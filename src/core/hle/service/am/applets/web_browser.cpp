// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/mode.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs_types.h"
#include "core/frontend/applets/web_browser.h"
#include "core/hle/kernel/process.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applets/web_browser.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace Service::AM::Applets {

namespace {

template <typename T>
void ParseRawValue(T& value, const std::vector<u8>& data) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "It's undefined behavior to use memcpy with non-trivially copyable objects");
    std::memcpy(&value, data.data(), data.size());
}

template <typename T>
T ParseRawValue(const std::vector<u8>& data) {
    T value;
    ParseRawValue(value, data);
    return value;
}

std::string ParseStringValue(const std::vector<u8>& data) {
    return Common::StringFromFixedZeroTerminatedBuffer(reinterpret_cast<const char*>(data.data()),
                                                       data.size());
}

std::string GetMainURL(const std::string& url) {
    const auto index = url.find('?');

    if (index == std::string::npos) {
        return url;
    }

    return url.substr(0, index);
}

WebArgInputTLVMap ReadWebArgs(const std::vector<u8>& web_arg, WebArgHeader& web_arg_header) {
    std::memcpy(&web_arg_header, web_arg.data(), sizeof(WebArgHeader));

    if (web_arg.size() == sizeof(WebArgHeader)) {
        return {};
    }

    WebArgInputTLVMap input_tlv_map;

    u64 current_offset = sizeof(WebArgHeader);

    for (std::size_t i = 0; i < web_arg_header.total_tlv_entries; ++i) {
        if (web_arg.size() < current_offset + sizeof(WebArgInputTLV)) {
            return input_tlv_map;
        }

        WebArgInputTLV input_tlv;
        std::memcpy(&input_tlv, web_arg.data() + current_offset, sizeof(WebArgInputTLV));

        current_offset += sizeof(WebArgInputTLV);

        if (web_arg.size() < current_offset + input_tlv.arg_data_size) {
            return input_tlv_map;
        }

        std::vector<u8> data(input_tlv.arg_data_size);
        std::memcpy(data.data(), web_arg.data() + current_offset, input_tlv.arg_data_size);

        current_offset += input_tlv.arg_data_size;

        input_tlv_map.insert_or_assign(input_tlv.input_tlv_type, std::move(data));
    }

    return input_tlv_map;
}

FileSys::VirtualFile GetOfflineRomFS(Core::System& system, u64 title_id,
                                     FileSys::ContentRecordType nca_type) {
    if (nca_type == FileSys::ContentRecordType::Data) {
        const auto nca =
            system.GetFileSystemController().GetSystemNANDContents()->GetEntry(title_id, nca_type);

        if (nca == nullptr) {
            LOG_ERROR(Service_AM,
                      "NCA of type={} with title_id={:016X} is not found in the System NAND!",
                      nca_type, title_id);
            return FileSys::SystemArchive::SynthesizeSystemArchive(title_id);
        }

        return nca->GetRomFS();
    } else {
        const auto nca = system.GetContentProvider().GetEntry(title_id, nca_type);

        if (nca == nullptr) {
            LOG_ERROR(Service_AM,
                      "NCA of type={} with title_id={:016X} is not found in the ContentProvider!",
                      nca_type, title_id);
            return nullptr;
        }

        const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};

        return pm.PatchRomFS(nca->GetRomFS(), nca->GetBaseIVFCOffset(), nca_type);
    }
}

} // namespace

WebBrowser::WebBrowser(Core::System& system_, const Core::Frontend::WebBrowserApplet& frontend_)
    : Applet{system_.Kernel()}, frontend(frontend_), system{system_} {}

WebBrowser::~WebBrowser() = default;

void WebBrowser::Initialize() {
    Applet::Initialize();

    LOG_INFO(Service_AM, "Initializing Web Browser Applet.");

    LOG_DEBUG(Service_AM,
              "Initializing Applet with common_args: arg_version={}, lib_version={}, "
              "play_startup_sound={}, size={}, system_tick={}, theme_color={}",
              common_args.arguments_version, common_args.library_version,
              common_args.play_startup_sound, common_args.size, common_args.system_tick,
              common_args.theme_color);

    web_applet_version = WebAppletVersion{common_args.library_version};

    const auto web_arg_storage = broker.PopNormalDataToApplet();
    ASSERT(web_arg_storage != nullptr);

    const auto& web_arg = web_arg_storage->GetData();
    ASSERT_OR_EXECUTE(web_arg.size() >= sizeof(WebArgHeader), { return; });

    web_arg_input_tlv_map = ReadWebArgs(web_arg, web_arg_header);

    LOG_DEBUG(Service_AM, "WebArgHeader: total_tlv_entries={}, shim_kind={}",
              web_arg_header.total_tlv_entries, web_arg_header.shim_kind);

    switch (web_arg_header.shim_kind) {
    case ShimKind::Shop:
        InitializeShop();
        break;
    case ShimKind::Login:
        InitializeLogin();
        break;
    case ShimKind::Offline:
        InitializeOffline();
        break;
    case ShimKind::Share:
        InitializeShare();
        break;
    case ShimKind::Web:
        InitializeWeb();
        break;
    case ShimKind::Wifi:
        InitializeWifi();
        break;
    case ShimKind::Lobby:
        InitializeLobby();
        break;
    default:
        UNREACHABLE_MSG("Invalid ShimKind={}", web_arg_header.shim_kind);
        break;
    }
}

bool WebBrowser::TransactionComplete() const {
    return complete;
}

ResultCode WebBrowser::GetStatus() const {
    return status;
}

void WebBrowser::ExecuteInteractive() {
    UNIMPLEMENTED_MSG("WebSession is not implemented");
}

void WebBrowser::Execute() {
    switch (web_arg_header.shim_kind) {
    case ShimKind::Shop:
        ExecuteShop();
        break;
    case ShimKind::Login:
        ExecuteLogin();
        break;
    case ShimKind::Offline:
        ExecuteOffline();
        break;
    case ShimKind::Share:
        ExecuteShare();
        break;
    case ShimKind::Web:
        ExecuteWeb();
        break;
    case ShimKind::Wifi:
        ExecuteWifi();
        break;
    case ShimKind::Lobby:
        ExecuteLobby();
        break;
    default:
        UNREACHABLE_MSG("Invalid ShimKind={}", web_arg_header.shim_kind);
        WebBrowserExit(WebExitReason::EndButtonPressed);
        break;
    }
}

void WebBrowser::WebBrowserExit(WebExitReason exit_reason, std::string last_url) {
    if ((web_arg_header.shim_kind == ShimKind::Share &&
         web_applet_version >= WebAppletVersion::Version196608) ||
        (web_arg_header.shim_kind == ShimKind::Web &&
         web_applet_version >= WebAppletVersion::Version524288)) {
        // TODO: Push Output TLVs instead of a WebCommonReturnValue
    }

    WebCommonReturnValue web_common_return_value;

    web_common_return_value.exit_reason = exit_reason;
    std::memcpy(&web_common_return_value.last_url, last_url.data(), last_url.size());
    web_common_return_value.last_url_size = last_url.size();

    LOG_DEBUG(Service_AM, "WebCommonReturnValue: exit_reason={}, last_url={}, last_url_size={}",
              exit_reason, last_url, last_url.size());

    complete = true;
    std::vector<u8> out_data(sizeof(WebCommonReturnValue));
    std::memcpy(out_data.data(), &web_common_return_value, out_data.size());
    broker.PushNormalDataFromApplet(std::make_shared<IStorage>(system, std::move(out_data)));
    broker.SignalStateChanged();
}

bool WebBrowser::InputTLVExistsInMap(WebArgInputTLVType input_tlv_type) const {
    return web_arg_input_tlv_map.find(input_tlv_type) != web_arg_input_tlv_map.end();
}

std::optional<std::vector<u8>> WebBrowser::GetInputTLVData(WebArgInputTLVType input_tlv_type) {
    const auto map_it = web_arg_input_tlv_map.find(input_tlv_type);

    if (map_it == web_arg_input_tlv_map.end()) {
        return std::nullopt;
    }

    return map_it->second;
}

void WebBrowser::InitializeShop() {}

void WebBrowser::InitializeLogin() {}

void WebBrowser::InitializeOffline() {
    const auto document_path =
        ParseStringValue(GetInputTLVData(WebArgInputTLVType::DocumentPath).value());

    const auto document_kind =
        ParseRawValue<DocumentKind>(GetInputTLVData(WebArgInputTLVType::DocumentKind).value());

    u64 title_id{};
    FileSys::ContentRecordType nca_type{FileSys::ContentRecordType::HtmlDocument};
    std::string additional_paths;

    switch (document_kind) {
    case DocumentKind::OfflineHtmlPage:
        title_id = system.CurrentProcess()->GetTitleID();
        nca_type = FileSys::ContentRecordType::HtmlDocument;
        additional_paths = "html-document";
        break;
    case DocumentKind::ApplicationLegalInformation:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::ApplicationID).value());
        nca_type = FileSys::ContentRecordType::LegalInformation;
        break;
    case DocumentKind::SystemDataPage:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::SystemDataID).value());
        nca_type = FileSys::ContentRecordType::Data;
        break;
    }

    static constexpr std::array<const char*, 3> RESOURCE_TYPES{
        "manual",
        "legal_information",
        "system_data",
    };

    offline_cache_dir = Common::FS::SanitizePath(
        fmt::format("{}/offline_web_applet_{}/{:016X}",
                    Common::FS::GetUserPath(Common::FS::UserPath::CacheDir),
                    RESOURCE_TYPES[static_cast<u32>(document_kind) - 1], title_id),
        Common::FS::DirectorySeparator::PlatformDefault);

    offline_document = Common::FS::SanitizePath(
        fmt::format("{}/{}/{}", offline_cache_dir, additional_paths, document_path),
        Common::FS::DirectorySeparator::PlatformDefault);

    const auto main_url = Common::FS::SanitizePath(GetMainURL(offline_document),
                                                   Common::FS::DirectorySeparator::PlatformDefault);

    if (Common::FS::Exists(main_url)) {
        return;
    }

    auto offline_romfs = GetOfflineRomFS(system, title_id, nca_type);

    if (offline_romfs == nullptr) {
        LOG_ERROR(Service_AM, "RomFS with title_id={:016X} and nca_type={} cannot be extracted!",
                  title_id, nca_type);
        return;
    }

    LOG_DEBUG(Service_AM, "Extracting RomFS to {}", offline_cache_dir);

    const auto extracted_romfs_dir =
        FileSys::ExtractRomFS(offline_romfs, FileSys::RomFSExtractionType::SingleDiscard);

    const auto temp_dir =
        system.GetFilesystem()->CreateDirectory(offline_cache_dir, FileSys::Mode::ReadWrite);

    FileSys::VfsRawCopyD(extracted_romfs_dir, temp_dir);
}

void WebBrowser::InitializeShare() {}

void WebBrowser::InitializeWeb() {}

void WebBrowser::InitializeWifi() {}

void WebBrowser::InitializeLobby() {}

void WebBrowser::ExecuteShop() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Shop Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLogin() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Login Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteOffline() {
    LOG_INFO(Service_AM, "Opening offline document at {}", offline_document);
    frontend.OpenLocalWebPage(offline_document,
                              [this](WebExitReason exit_reason, std::string last_url) {
                                  WebBrowserExit(exit_reason, last_url);
                              });
}

void WebBrowser::ExecuteShare() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Share Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteWeb() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Web Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteWifi() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Wifi Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLobby() {
    LOG_WARNING(Service_AM, "(STUBBED) called, Lobby Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}
} // namespace Service::AM::Applets
