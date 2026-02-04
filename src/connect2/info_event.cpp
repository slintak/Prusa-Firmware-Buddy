#include "info_event.hpp"

#include <connect/printer.hpp>
#include <connect/printer_common.hpp>
#include <state/printer_state.hpp>

#include <filament.hpp>
#include <filament_list.hpp>

#include <pb_encode.h>

#include <transfers/changed_path.hpp>
#include <common/filepath_operation.h>
#include <common/filename_type.hpp>
#include <common/lfn.h>
#include <common/mutable_path.hpp>
#include <unique_dir_ptr.hpp>
#include <gui/file_list_defs.h>

#include "info_event.pb.h"

#include <option/has_chamber_filtration_api.h>
#if HAS_CHAMBER_FILTRATION_API()
    #include <feature/chamber_filtration/chamber_filtration.hpp>
#endif

#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace connect2_client {

namespace {

void set_str(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

bool path_allowed(const char *path) {
    constexpr const char *const usb = "/usb/";
    const bool is_on_usb = strncmp(path, usb, strlen(usb)) == 0 || strcmp(path, "/usb") == 0;
    const bool contains_upper = strstr(path, "/../") != nullptr;
    return is_on_usb && !contains_upper;
}

std::optional<off_t> child_size(const char *base_path, const char *child_name) {
    char path_buf[FILE_PATH_BUFFER_LEN];
    int formatted = snprintf(path_buf, sizeof(path_buf), "%s/%s", base_path, child_name);
    if (formatted >= FILE_NAME_BUFFER_LEN) {
        return {};
    }
    struct stat st = {};
    if (stat(path_buf, &st) == 0) {
        return st.st_size;
    }
    return {};
}

void get_display_name_from_path(const char *sfn_path, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    char path_buf[FILE_PATH_BUFFER_LEN] = {};
    snprintf(path_buf, sizeof(path_buf), "%s", sfn_path);
    get_LFN(out, out_size, path_buf);
    if (out[0] == '\0') {
        set_str(out, out_size, basename_b(sfn_path));
    }
}

bool fill_file_entry_from_dirent(const char *base_path, struct dirent *ent, FileEntry &entry) {
    if (const char *lfn = dirent_lfn(ent); lfn && lfn[0] == '.') {
        return false;
    }

    std::optional<off_t> size = child_size(base_path, ent->d_name);
    const bool read_only = false;

    set_str(entry.name, sizeof(entry.name), ent->d_name);
    set_str(entry.display_name, sizeof(entry.display_name), dirent_lfn(ent));
    entry.size = size.value_or(0);
#ifdef UNITTESTS
    entry.m_timestamp = 0;
#else
    entry.m_timestamp = ent->time;
#endif
    entry.read_only = read_only;
    set_str(entry.type, sizeof(entry.type), file_type(ent));
    return true;
}

} // namespace

bool encode_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    size_t &out_size,
    uint32_t command_id) {
    InfoEvent msg = InfoEvent_init_zero;

    set_str(msg.event, sizeof(msg.event), "INFO");
    set_str(msg.state, sizeof(msg.state), printer_state::to_str(params.state.device_state));
    if (params.state.dialog.has_value()) {
        msg.dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    }
    msg.command_id = command_id;

    msg.has_data = true;
    InfoData &data = msg.data;
    const auto &info = printer.printer_info();
    const auto creds = printer.net_creds();

    char printer_type[16];
    snprintf(printer_type, sizeof(printer_type), "%hhu.%hhu.%hhu", params.version.type, params.version.version, params.version.subversion);

    set_str(data.firmware, sizeof(data.firmware), info.firmware_version);
    set_str(data.printer_type, sizeof(data.printer_type), printer_type);
    set_str(data.sn, sizeof(data.sn), info.serial_number.begin());
    data.appendix = info.appendix;
    set_str(data.fingerprint, sizeof(data.fingerprint), info.fingerprint);
    data.nozzle_diameter = params.slots[params.preferred_head()].nozzle_diameter;
    data.transfer_paused = !params.can_start_download;
    if (creds.pl_password[0] != '\0') {
        set_str(data.api_key, sizeof(data.api_key), creds.pl_password);
    }

    data.storages_count = 0;
    if (params.has_usb) {
        Storage &s = data.storages[0];
        set_str(s.mountpoint, sizeof(s.mountpoint), "/usb");
        set_str(s.type, sizeof(s.type), "USB");
        s.read_only = false;
        s.free_space = params.usb_space_free;
        s.is_sfn = true;
        data.storages_count = 1;
    }

    msg.data.has_network_info = true;
    NetworkInfo &net = data.network_info;
    if (const auto lan = printer.net_info(connect_client::Printer::Iface::Ethernet); lan.has_value()) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
            lan->mac[0], lan->mac[1], lan->mac[2], lan->mac[3], lan->mac[4], lan->mac[5]);
        set_str(net.lan_mac, sizeof(net.lan_mac), tmp);
        snprintf(tmp, sizeof(tmp), "%hhu.%hhu.%hhu.%hhu", lan->ip[0], lan->ip[1], lan->ip[2], lan->ip[3]);
        set_str(net.lan_ipv4, sizeof(net.lan_ipv4), tmp);
    }
    if (const auto wifi = printer.net_info(connect_client::Printer::Iface::Wifi); wifi.has_value()) {
        if (creds.ssid[0] != '\0') {
            set_str(net.wifi_ssid, sizeof(net.wifi_ssid), creds.ssid);
        }
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
            wifi->mac[0], wifi->mac[1], wifi->mac[2], wifi->mac[3], wifi->mac[4], wifi->mac[5]);
        set_str(net.wifi_mac, sizeof(net.wifi_mac), tmp);
        snprintf(tmp, sizeof(tmp), "%hhu.%hhu.%hhu.%hhu", wifi->ip[0], wifi->ip[1], wifi->ip[2], wifi->ip[3]);
        set_str(net.wifi_ipv4, sizeof(net.wifi_ipv4), tmp);
    }
    set_str(net.hostname, sizeof(net.hostname), creds.hostname);

    data.tools_count = 0;
    for (size_t i = 0; i < connect_client::Printer::NUMBER_OF_SLOTS; i++) {
        if (params.slot_mask & (1 << i)) {
            Tool &t = data.tools[data.tools_count++];
            t.slot = static_cast<uint32_t>(i + 1);
            t.nozzle_diameter = params.slots[i].nozzle_diameter;
            t.high_flow = params.slots[i].high_flow;
            t.hardened = params.slots[i].hardened;
            const char *material = *params.slots[i].material.data() ? params.slots[i].material.data() : "---";
            set_str(t.material, sizeof(t.material), material);
        }
    }

#if XL_ENCLOSURE_SUPPORT()
    if (params.enclosure_info.present) {
        data.has_enclosure = true;
        Enclosure &e = data.enclosure;
        e.enabled = params.enclosure_info.enabled;
        e.printing_filtration = params.enclosure_info.printing_filtration;
        e.post_print = params.enclosure_info.post_print;
        e.post_print_filtration_time = params.enclosure_info.post_print_filtration_time;
#if HAS_CHAMBER_FILTRATION_API()
        e.filter_lifetime = buddy::chamber_filtration().filter_lifetime_s();
        e.filtration_filaments_count = 0;
        for (size_t i = 0; i < all_filament_types.size() && e.filtration_filaments_count < Enclosure_filtration_filaments_max_count; i++) {
            if (!all_filament_types[i].parameters().requires_filtration) {
                continue;
            }
            set_str(e.filtration_filaments[e.filtration_filaments_count++],
                sizeof(e.filtration_filaments[0]),
                all_filament_types[i].parameters().name.data());
        }
#endif
    }
#endif

#if HAS_MMU2()
    data.has_mmu = true;
    Mmu &m = data.mmu;
    m.enabled = params.enabled_tool_cnt() > 1;
    char ver[16];
    snprintf(ver, sizeof(ver), "%d.%d.%d", params.mmu_version.major, params.mmu_version.minor, params.mmu_version.build);
    set_str(m.version, sizeof(m.version), ver);
#endif

#if PRINTER_IS_PRUSA_COREONE()
    data.addon_power = params.addon_power;
#endif

    data.slots = params.enabled_tool_cnt();

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
    if (!pb_encode(&stream, InfoEvent_fields, &msg)) {
        return false;
    }
    out_size = stream.bytes_written;
    return true;
}

bool encode_file_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *path,
    size_t &out_size,
    uint32_t command_id) {
    (void)printer;
    if (path == nullptr || path[0] == '\0' || !path_allowed(path)) {
        return false;
    }

    char sfn_path[FILE_PATH_BUFFER_LEN] = {};
    snprintf(sfn_path, sizeof(sfn_path), "%s", path);
    get_SFN_path(sfn_path);

    struct stat st = {};
    bool has_stat = false;
    bool read_only = false;
    bool is_dir = false;

    if (stat(sfn_path, &st) == 0) {
        has_stat = true;
    }

    unique_dir_ptr dir(opendir(sfn_path));
    if (dir.get() != nullptr) {
        is_dir = true;
    } else if (!has_stat) {
        return false;
    }

    FileInfoEvent msg = FileInfoEvent_init_zero;
    set_str(msg.event, sizeof(msg.event), "FILE_INFO");
    set_str(msg.state, sizeof(msg.state), printer_state::to_str(params.state.device_state));
    if (params.state.dialog.has_value()) {
        msg.dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    }
    msg.command_id = command_id;
    msg.has_file_info = true;
    FileInfo &fi = msg.file_info;

    set_str(fi.path, sizeof(fi.path), sfn_path);
    get_display_name_from_path(sfn_path, fi.display_name, sizeof(fi.display_name));
    set_str(fi.type, sizeof(fi.type), is_dir ? "FOLDER" : file_type_by_ext(sfn_path));
    fi.read_only = read_only;
    if (has_stat) {
        fi.size = st.st_size;
        fi.m_timestamp = st.st_mtime;
    }

    fi.children_count = 0;
    fi.file_count = 0;
    if (is_dir) {
        struct dirent *ent = nullptr;
        while (dir.get() && (ent = readdir(dir.get())) != nullptr) {
            fi.file_count++;
            const size_t max_children = sizeof(fi.children) / sizeof(fi.children[0]);
            // NOTE: protobuf response is capped (currently 8 entries via nanopb max_count).
            // If we need full listings like src/connect JSON, emit paged/streamed FILE_INFO
            // events (page index + more flag) instead of a single large message.
            if (fi.children_count >= max_children) {
                continue;
            }
            FileEntry &entry = fi.children[fi.children_count];
            if (fill_file_entry_from_dirent(sfn_path, ent, entry)) {
                fi.children_count++;
            }
        }
    }

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
    if (!pb_encode(&stream, FileInfoEvent_fields, &msg)) {
        return false;
    }
    out_size = stream.bytes_written;
    return true;
}

bool encode_file_changed_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *path,
    bool is_file,
    int incident,
    size_t &out_size,
    uint32_t command_id) {
    (void)printer;
    if (path == nullptr || path[0] == '\0' || !path_allowed(path)) {
        return false;
    }

    char sfn_path[FILE_PATH_BUFFER_LEN] = {};
    snprintf(sfn_path, sizeof(sfn_path), "%s", path);
    get_SFN_path(sfn_path);

    FileChangedEvent msg = FileChangedEvent_init_zero;
    set_str(msg.event, sizeof(msg.event), "FILE_CHANGED");
    set_str(msg.state, sizeof(msg.state), printer_state::to_str(params.state.device_state));
    if (params.state.dialog.has_value()) {
        msg.dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    }
    msg.command_id = command_id;
    msg.has_file_changed = true;
    FileChanged &fc = msg.file_changed;
    fc.has_file = true;

    if (params.has_usb) {
        fc.free_space = params.usb_space_free;
    }
    const auto incident_enum = static_cast<transfers::ChangedPath::Incident>(incident);
    if (incident_enum == transfers::ChangedPath::Incident::Created) {
        set_str(fc.new_path, sizeof(fc.new_path), sfn_path);
    } else if (incident_enum == transfers::ChangedPath::Incident::Deleted) {
        set_str(fc.old_path, sizeof(fc.old_path), sfn_path);
    } else {
        set_str(fc.new_path, sizeof(fc.new_path), sfn_path);
        fc.rescan = true;
    }

    FileEntry &entry = fc.file;
    set_str(entry.name, sizeof(entry.name), basename_b(sfn_path));
    get_display_name_from_path(sfn_path, entry.display_name, sizeof(entry.display_name));
    set_str(entry.type, sizeof(entry.type), is_file ? file_type_by_ext(sfn_path) : "FOLDER");
    struct stat st = {};
    if (stat(sfn_path, &st) == 0) {
        entry.size = st.st_size;
        entry.m_timestamp = st.st_mtime;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
    if (!pb_encode(&stream, FileChangedEvent_fields, &msg)) {
        return false;
    }
    out_size = stream.bytes_written;
    return true;
}

bool encode_rejected_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    const char *reason,
    size_t &out_size,
    uint32_t command_id) {
    (void)printer;
    if (reason == nullptr || reason[0] == '\0') {
        return false;
    }

    RejectedEvent msg = RejectedEvent_init_zero;
    set_str(msg.event, sizeof(msg.event), "REJECTED");
    set_str(msg.state, sizeof(msg.state), printer_state::to_str(params.state.device_state));
    if (params.state.dialog.has_value()) {
        msg.dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    }
    msg.command_id = command_id;
    msg.has_rejected = true;
    set_str(msg.rejected.reason, sizeof(msg.rejected.reason), reason);

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, buffer_size);
    if (!pb_encode(&stream, RejectedEvent_fields, &msg)) {
        return false;
    }
    out_size = stream.bytes_written;
    return true;
}

} // namespace connect2_client
