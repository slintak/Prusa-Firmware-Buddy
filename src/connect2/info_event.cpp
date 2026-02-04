#include "info_event.hpp"

#include <connect/printer.hpp>
#include <connect/printer_common.hpp>
#include <state/printer_state.hpp>

#include <filament.hpp>
#include <filament_list.hpp>

#include <pb_encode.h>

#include "info_event.pb.h"

#include <option/has_chamber_filtration_api.h>
#if HAS_CHAMBER_FILTRATION_API()
    #include <feature/chamber_filtration/chamber_filtration.hpp>
#endif

#include <cstring>
#include <cstdio>

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

} // namespace

bool encode_info_event(uint8_t *buffer, size_t buffer_size,
    const connect_client::Printer &printer,
    const connect_client::Printer::Params &params,
    size_t &out_size) {
    InfoEvent msg = InfoEvent_init_zero;

    set_str(msg.event, sizeof(msg.event), "INFO");
    set_str(msg.state, sizeof(msg.state), printer_state::to_str(params.state.device_state));
    if (params.state.dialog.has_value()) {
        msg.dialog_id = params.state.dialog->dialog_id.to_uint32_t();
    }

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

} // namespace connect2_client
