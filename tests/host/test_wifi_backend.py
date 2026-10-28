#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile actual backend functions with deterministic hardware-free Zephyr fakes.

Run directly; does not change run.sh, firmware, NVS or host build directories.
The fakes assert that no driver/NVS call runs while server_lock is held.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/papermono_content_server.c").read_text()


def function(name):
    import re
    match = re.search(r"^(?:static )?(?:enum \w+|void|int|bool) " + name + r"\([^;]*?\)\n\{", SOURCE, re.M)
    if not match:
        raise AssertionError(f"missing C function: {name}")
    start = match.start()
    depth = 0
    for index in range(SOURCE.index("{", match.start()), len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:index + 1] + "\n"
    raise AssertionError(f"unclosed C function: {name}")


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <papermono/content_server.h>
#define PAPERMONO_WIFI_SSID_MAX 32
#define PAPERMONO_WIFI_PSK_MAX 64
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_SECONDS(x) ((x)*1000)
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_FREQ_BAND_2_4_GHZ 0
#define WIFI_CHANNEL_ANY 0
#define WIFI_MFP_DISABLE 0
#define WIFI_MFP_OPTIONAL 1
#define NET_REQUEST_WIFI_SCAN 1
#define NET_REQUEST_WIFI_CONNECT 2
#define NET_REQUEST_WIFI_DISCONNECT 3
#define NET_REQUEST_WIFI_AP_DISABLE 4
#define LOG_WRN(...) ((void)0)
typedef int k_timeout_t;
struct k_work { int unused; };
struct k_work_delayable { int unused; };
struct k_work_q { int unused; };
struct net_if { int unused; };
enum wifi_security_type { WIFI_SECURITY_TYPE_NONE, WIFI_SECURITY_TYPE_PSK, WIFI_SECURITY_TYPE_SAE };
struct wifi_scan_result { uint8_t ssid[33]; uint8_t ssid_length, band, channel; enum wifi_security_type security; int8_t rssi; };
struct wifi_scan_params { int scan_type; };
struct wifi_connect_req_params { const uint8_t *ssid, *psk; uint8_t ssid_length, psk_length, channel, band; enum wifi_security_type security; int timeout; int mfp; };
struct papermono_preferences { char wifi_ssid[33], wifi_psk[65]; bool wifi_configured; };
static struct papermono_preferences preferences;
static struct papermono_wifi_state wifi_state;
static struct papermono_content_server_state server_state;
static struct papermono_wifi_network scan_networks[PAPERMONO_WIFI_NETWORK_MAX];
static uint8_t scan_count;
static bool scan_inflight, initialized = true, ap_enabling;
static struct k_work scan_work;
static struct k_work_q wifi_work_queue;
static struct k_work_delayable scan_timeout_work, wifi_command_work, station_retry_work, apply_wifi_work, disable_ap_work, enable_ap_work;
static int server_lock, lock_depth, refreshes, scheduled, retries, recovery, reset_calls;
static int wifi_paused;
static bool wifi_is_paused(void) { return wifi_paused != 0; }
static int driver_result, nvs_result, writes, disconnects, scan_mode;
static int64_t now;
static struct net_if station_iface, ap_iface;
enum wifi_command { WIFI_COMMAND_NONE, WIFI_COMMAND_CONNECT, WIFI_COMMAND_SAVED, WIFI_COMMAND_FORGET, WIFI_COMMAND_RESET };
static enum wifi_command pending_wifi_command;
static uint8_t wifi_command_phase;
static int64_t wifi_command_deadline;
static char pending_wifi_ssid[33], pending_wifi_psk[65];
static void k_mutex_lock(int *m, int t) { (void)m; (void)t; assert(lock_depth == 0); ++lock_depth; }
static void k_mutex_unlock(int *m) { (void)m; assert(lock_depth == 1); --lock_depth; }
static struct net_if *net_if_get_wifi_sta(void) { return &station_iface; }
static struct net_if *net_if_get_wifi_sap(void) { return &ap_iface; }
static const struct papermono_preferences *papermono_settings_get(void) { return &preferences; }
static int papermono_settings_set_wifi(const char *ssid, const char *psk) {
 assert(!lock_depth); ++writes; if(nvs_result) return nvs_result;
 snprintf(preferences.wifi_ssid, sizeof(preferences.wifi_ssid), "%s", ssid);
 snprintf(preferences.wifi_psk, sizeof(preferences.wifi_psk), "%s", psk);
 preferences.wifi_configured = true; return 0;
}
static int papermono_settings_clear_wifi(void) {
 assert(!lock_depth); ++writes; if(nvs_result) return nvs_result;
 memset(&preferences, 0, sizeof(preferences)); return 0;
}
static void papermono_ui_request_background_refresh(void) { assert(!lock_depth); ++refreshes; }
static int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w) { assert(q==&wifi_work_queue); (void)w; ++scheduled; return 1; }
static int k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *w, int d) { assert(q==&wifi_work_queue); (void)w; (void)d; ++scheduled; return 1; }
static int k_work_cancel_delayable(struct k_work_delayable *w) { (void)w; return 0; }
static int64_t k_uptime_get(void) { return now; }
static void schedule_station_retry(void) { assert(!lock_depth); ++retries; }
static void schedule_recovery_ap(void) { assert(!lock_depth); ++recovery; }
static int reset_wifi_now(void) { assert(!lock_depth); ++reset_calls; wifi_state.connection=PAPERMONO_WIFI_DISCONNECTED; return 0; }
static void finish_scan_locked(int error);
static int net_mgmt(int request, struct net_if *iface, void *data, size_t size) {
 assert(!lock_depth); (void)iface; (void)size;
 if(request == NET_REQUEST_WIFI_DISCONNECT) ++disconnects;
 if(request == NET_REQUEST_WIFI_CONNECT) {
   struct wifi_connect_req_params *c=data;
   assert(c->ssid_length > 0 && c->ssid_length <= 32);
   assert(c->psk_length <= 63);
   assert(c->security == (c->psk_length ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE));
 }
 /* Emulate DONE synchronously inside the scan request, before return. */
 if(request == NET_REQUEST_WIFI_SCAN && scan_mode) {
   k_mutex_lock(&server_lock,0); scan_inflight=false; finish_scan_locked(0); k_mutex_unlock(&server_lock);
 }
 return driver_result;
}
'''

MAIN = r'''
static struct papermono_wifi_network candidate(void) {
 struct papermono_wifi_network n={.ssid="test-network",.ssid_length=12,.security=PAPERMONO_WIFI_SECURITY_WPA2_PSK,.supported=true}; return n;
}
static void clean(void) {
 memset(&wifi_state,0,sizeof(wifi_state)); memset(&server_state,0,sizeof(server_state));
 memset(&preferences,0,sizeof(preferences)); memset(scan_networks,0,sizeof(scan_networks));
 pending_wifi_command=WIFI_COMMAND_NONE; wifi_command_phase=0;
 scan_count=0; scan_inflight=false; ap_enabling=false; nvs_result=driver_result=scan_mode=0; now=0;
 refreshes=writes=disconnects=scheduled=reset_calls=0;
}
int main(void) {
 clean();
 struct wifi_scan_result r={.ssid_length=32,.band=0,.channel=6,.security=WIFI_SECURITY_TYPE_PSK,.rssi=-70};
 memset(r.ssid,'x',sizeof(r.ssid)); scan_add_locked(&r);
 assert(scan_count==1 && scan_networks[0].ssid[32]==0 && scan_networks[0].ssid_length==32 && scan_networks[0].supported);
 r.rssi=-30; scan_add_locked(&r); assert(scan_count==1 && scan_networks[0].rssi==-30);
 r.rssi=-90; scan_add_locked(&r); assert(scan_networks[0].rssi==-30);
 r.ssid_length=0; scan_add_locked(&r); assert(scan_networks[1].hidden && !scan_networks[1].supported);
 r.ssid_length=3; memcpy(r.ssid,"a\0b",3); scan_add_locked(&r); assert(!scan_networks[2].supported);
 r.ssid_length=2; memcpy(r.ssid,"sa",2); r.security=WIFI_SECURITY_TYPE_SAE; scan_add_locked(&r);
 assert(scan_networks[3].security==PAPERMONO_WIFI_SECURITY_UNSUPPORTED && !scan_networks[3].supported);
 clean();
 for(int i=0;i<40;++i) { r.ssid_length=2; r.ssid[0]='A'+i; r.ssid[1]='x'; r.security=WIFI_SECURITY_TYPE_NONE; r.rssi=-100+i; scan_add_locked(&r); }
 assert(scan_count==16); for(int i=1;i<16;++i) assert(scan_networks[i-1].rssi>=scan_networks[i].rssi);
 assert(scan_networks[0].rssi==-61 && scan_networks[15].rssi==-76);
 finish_scan_locked(0); assert(wifi_state.count==16 && wifi_state.scan_generation==1);
 clean(); assert(papermono_content_server_scan_wifi()==0 && wifi_state.scan_busy);
 assert(papermono_content_server_scan_wifi()==-EBUSY);
 driver_result=7; scan_wifi_worker(NULL); assert(!wifi_state.scan_busy && !scan_inflight && wifi_state.scan_error==-EIO);
 clean(); papermono_content_server_scan_wifi(); scan_mode=1; scan_wifi_worker(NULL);
 assert(!wifi_state.scan_busy && !scan_inflight && wifi_state.scan_generation==1);
 clean(); papermono_content_server_scan_wifi(); scan_timeout_worker(NULL);
 assert(wifi_state.scan_error==-ETIMEDOUT && !wifi_state.scan_busy && scan_inflight);
 assert(papermono_content_server_scan_wifi()==-EBUSY); /* quarantine late scan events */
 clean(); struct papermono_wifi_network n=candidate(); char password[]="dynamic-psk";
 assert(papermono_content_server_connect_wifi(&n,password)==0);
 assert(writes==0 && wifi_state.request_busy); memset(password,0,sizeof(password));
 assert(papermono_content_server_connect_wifi(&n,"another-psk")==-EBUSY);
 wifi_command_worker(NULL); assert(writes==1 && wifi_state.request_busy && server_state.station_connecting);
 assert(!strcmp(preferences.wifi_psk,"dynamic-psk") && pending_wifi_psk[0]==0);
 server_state.station_connecting=false; server_state.station_connected=true;
 wifi_state.connection=PAPERMONO_WIFI_CONNECTED; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.connection==PAPERMONO_WIFI_CONNECTED);
 clean(); n.security=PAPERMONO_WIFI_SECURITY_OPEN;
 assert(papermono_content_server_connect_wifi(&n,"")==0); wifi_command_worker(NULL);
 assert(preferences.wifi_psk[0]==0 && server_state.station_connecting);
 clean(); n=candidate(); n.hidden=true; assert(papermono_content_server_connect_wifi(&n,"valid-psk")==-ENOTSUP);
 n.hidden=false; assert(papermono_content_server_connect_wifi(&n,"short")==-EINVAL);
 assert(papermono_content_server_connect_wifi(&n,"1234567\n")==-EINVAL);
 char longkey[65]; memset(longkey,'a',64); longkey[64]=0; assert(papermono_content_server_connect_wifi(&n,longkey)==-EINVAL);
 clean(); server_state.station_connected=true; nvs_result=-EIO;
 assert(papermono_content_server_connect_wifi(&n,"valid-psk")==0); wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.last_connection_error==-EIO && !disconnects && server_state.station_connected);
 clean(); preferences.wifi_configured=true; strcpy(preferences.wifi_ssid,"saved"); strcpy(preferences.wifi_psk,"saved-pass");
 wifi_state.configured=true; assert(papermono_content_server_connect_saved()==0); wifi_command_worker(NULL);
 assert(writes==0 && server_state.station_connecting); now=40000; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && wifi_state.last_connection_error==-ETIMEDOUT);
 clean(); server_state.station_connected=true; server_state.ap_enabled=true;
 assert(papermono_content_server_forget_wifi()==0); wifi_command_worker(NULL);
 assert(wifi_state.request_busy && disconnects==1 && !reset_calls);
 server_state.station_connected=false; wifi_command_worker(NULL); assert(wifi_command_phase==3 && !reset_calls);
 server_state.ap_enabled=false; wifi_command_worker(NULL); assert(reset_calls==1 && !wifi_state.request_busy);
 clean(); ap_enabling=true;
 assert(papermono_content_server_forget_wifi()==0); wifi_command_worker(NULL);
 assert(wifi_state.request_busy && wifi_command_phase==1 && reset_calls==0);
 ap_enabling=false; server_state.ap_enabled=true; wifi_command_worker(NULL);
 assert(wifi_state.request_busy && wifi_command_phase==3 && reset_calls==0);
 server_state.ap_enabled=false; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && reset_calls==1);
 clean(); ap_enabling=true; papermono_content_server_forget_wifi(); wifi_command_worker(NULL);
 now=6000; wifi_command_worker(NULL);
 assert(!wifi_state.request_busy && reset_calls==0 && wifi_state.last_connection_error==-ETIMEDOUT);
 puts("wifi backend: scan, copy, async, lock, NVS, reconnect, forget + AP-start race PASS");
 return 0;
}
'''


class WifiBackendTests(unittest.TestCase):
    def test_actual_c_backend(self):
        names = ["wifi_reschedule", "wipe_wifi_secret", "connect_station", "scan_security",
                 "scan_add_locked", "finish_scan_locked", "scan_wifi_worker", "scan_timeout_worker",
                 "wifi_command_complete", "wifi_command_worker", "papermono_content_server_get_wifi_state",
                 "papermono_content_server_scan_wifi", "submit_wifi_command_locked",
                 "papermono_content_server_connect_wifi", "papermono_content_server_connect_saved",
                 "papermono_content_server_forget_wifi"]
        code = PREFIX + "\n".join(function(name) for name in names) + MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-wifi-") as directory:
            source = Path(directory) / "wifi.c"
            executable = Path(directory) / "wifi-test"
            source.write_text(code)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-variable", "-I", str(ROOT / "include"), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_no_system_queue_or_direct_http_nvs(self):
        # Wi-Fi transitions and NVS must use their dedicated queue. The RSS
        # deadline is the only system-queue exception: its callback only checks
        # short state and shuts down a socket, leaving cleanup to HTTP ABORT.
        system_calls = re.finditer(
            r"\bk_work_(?:reschedule|schedule|submit)\s*\(\s*([^,)]+)", SOURCE)
        for call in system_calls:
            with self.subTest(system_work=call.group(1).strip()):
                self.assertEqual(call.group(1).strip(), "&rss_request_timeout",
                                 "Wi-Fi work must stay on wifi_work_queue")
        self.assertIn("k_work_reschedule_for_queue(&wifi_work_queue", function("wifi_reschedule"))
        deadline = function("rss_request_timeout_worker")
        self.assertIn("K_NO_WAIT", deadline)
        self.assertNotIn("K_FOREVER", deadline)
        for forbidden in ("papermono_settings_", "net_mgmt(", "papermono_content_store_",
                          "papermono_ui_", "papermono_rss_sources_replace", "k_msleep("):
            self.assertNotIn(forbidden, deadline)
        self.assertNotIn("papermono_settings_set_wifi", function("wifi_handler"))
        self.assertNotIn("net_mgmt(", function("papermono_content_server_connect_wifi"))
        self.assertNotIn("net_mgmt(", function("papermono_content_server_scan_wifi"))
        self.assertNotIn("psk", (ROOT / "include/papermono/content_server.h").read_text().split("struct papermono_wifi_state {")[1].split("};")[0])

    def test_event_integrity_and_boot(self):
        events = function("wifi_event")
        self.assertIn("callback->info_length >= sizeof(struct wifi_scan_result)", events)
        self.assertIn("iface != net_if_get_wifi_sta()", events)
        self.assertIn("wifi_state.scan_busy", events)
        self.assertIn("const bool intentional", events)
        boot = function("papermono_content_server_init")
        self.assertNotIn("ret = connect_station()", boot)
        self.assertIn("wifi_reschedule(&apply_wifi_work", boot)


if __name__ == "__main__":
    unittest.main()
