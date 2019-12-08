// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// help.h: main program help header
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef HELP_H
#define HELP_H

#include <argp.h>
const char *argp_program_bug_address = "Michael Wolf <michael@mictronics.de>";
static error_t parse_opt (int key, char *arg, struct argp_state *state);

static struct argp_option options[] =
{
    {0,0,0,0, "General options:", 1},
#if defined(READSB) || defined(VIEWADSB)
    {"lat", OptLat, "<lat>", 0, "Reference/receiver surface latitude", 1},
    {"lon", OptLon, "<lon>", 0, "Reference/receiver surface longitude", 1},
    {"no-interactive", OptNoInteractive, 0, 0, "Disable interactive mode, print to stdout", 1},
    {"interactive-ttl", OptInteractiveTTL, "<sec>", 0, "Remove from list if idle for <sec> (default: 60)", 1},
    {"modeac", OptModeAc, 0, 0, "Enable decoding of SSR Modes 3/A & 3/C", 1},
    {"max-range", OptMaxRange, "<dist>", 0, "Absolute maximum range for position decoding (in nm, default: 300)", 1},
    {"fix", OptFix, 0, 0, "Enable CRC single-bit error correction (default)", 1},
    {"no-fix", OptNoFix, 0, 0, "Disable CRC single-bit error correction", 1},
    {"no-crc-check", OptNoCrcCheck, 0, 0, "Disable messages with invalid CRC (discouraged)", 1},
    {"metric", OptMetric, 0, 0, "Use metric units", 1},
    {"show-only", OptShowOnly, "<addr>", 0, "Show only messages by given ICAO on stdout", 1},
    #ifdef ALLOW_AGGRESSIVE
        {"aggressive", OptAggressive, 0, 0, "Enable two-bit CRC error correction", 1},
    #else
        {"aggressive", OptAggressive, 0, OPTION_HIDDEN, "Enable two-bit CRC error correction", 1},
    #endif
#endif
#if defined(READSB)
    {"device-type", OptDeviceType, "<type>", 0, "Select SDR type", 1},
    {"gain", OptGain, "<db>", 0, "Set gain (default: max gain. Use -10 for auto-gain)", 1},
    {"freq", OptFreq, "<hz>", 0, "Set frequency (default: 1090 MHz)", 1},
    {"interactive", OptInteractive, 0, 0, "Interactive mode refreshing data on screen. Implies --throttle", 1},
    {"raw", OptRaw, 0, 0, "Show only messages hex values", 1},
    {"no-modeac-auto", OptNoModeAcAuto, 0, 0, "Don't enable Mode A/C if requested by a Beast connection", 1},
    {"forward-mlat", OptForwardMlat, 0, 0, "Allow forwarding of received mlat results to output ports", 1},
    {"mlat", OptMlat, 0, 0, "Display raw messages in Beast ASCII mode", 1},
    {"stats", OptStats, 0, 0, "With --ifile print stats at exit. No other output", 1},
    {"stats-range", OptStatsRange, 0, 0, "Collect/show range histogram", 1},
    {"stats-every", OptStatsEvery, "<sec>", 0, "Show and reset stats every <sec> seconds", 1},
    {"onlyaddr", OptOnlyAddr, 0, 0, "Show only ICAO addresses", 1},
    {"gnss", OptGnss, 0, 0, "Show altitudes as GNSS when available", 1},
    {"snip", OptSnip, "<level>", 0, "Strip IQ file removing samples < level", 1},
    {"debug", OptDebug, "<flags>", 0, "Debug mode (verbose), see flags below", 1},
    {"quiet", OptQuiet, 0, 0, "Disable output. Use for daemon applications", 1},
    {"dcfilter", OptDcFilter, 0, 0, "Apply a 1Hz DC filter to input data (requires more CPU)", 1},
    {"enable-biastee", OptBiasTee, 0, 0, "Enable bias tee on supporting interfaces (default: disabled)", 1},
    #ifndef _WIN32
        {"write-json", OptJsonDir, "<dir>", 0, "Periodically write json output to <dir> (for external webserver)", 1},
        {"write-json-every", OptJsonTime, "<t>", 0, "Write json output every t seconds (default 1)", 1},
        {"json-location-accuracy", OptJsonLocAcc , "<n>", 0, "Accuracy of receiver location in json metadata: 0=no location, 1=approximate, 2=exact", 1},
    #endif
#endif
    {0,0,0,0, "Network options:", 2},
#if defined(READSB) || defined(VIEWADSB)
    {"net-bind-address", OptNetBindAddr, "<ip>", 0, "IP address to bind to (default: Any; Use 127.0.0.1 for private)", 2},
    {"net-bo-port", OptNetBoPorts, "<ports>", 0, "TCP Beast output listen ports (default: 30005)", 2},
#endif
#if defined(READSB)
    {"net", OptNet, 0, 0, "Enable networking", 2},
    {"net-only", OptNetOnly, 0, 0, "Enable just networking, no RTL device or file used", 2},
    {"net-ri-port", OptNetRiPorts, "<ports>", 0, "TCP raw input listen ports  (default: 30001)", 2},
    {"net-ro-port", OptNetRoPorts, "<ports>", 0, "TCP raw output listen ports (default: 30002)", 2},
    {"net-sbs-port", OptNetSbsPorts, "<ports>", 0, "TCP BaseStation output listen ports (default: 30003)", 2},
    {"net-bi-port", OptNetBiPorts, "<ports>", 0, "TCP Beast input listen ports  (default: 30004,30104)", 2},
    {"net-vrs-port", OptNetVRSPorts, "<ports>", 0, "TCP VRS json output listen ports (default: 0)", 2},
    {"net-ro-size", OptNetRoSize, "<size>", 0, "TCP output minimum size (default: 0)", 2},
    {"net-ro-interval", OptNetRoIntervall, "<rate>", 0, "TCP output memory flush rate in seconds (default: 0)", 2},
    {"net-connector", OptNetConnector, "<ip:port:protocol>", 0, "Establish connection, can be specified multiple times (example: 127.0.0.1:23004:beast_out) Protocols: beast_out, beast_in, raw_out, raw_in, sbs_out, vrs_out", 2},
    {"net-push-address", OptNetPushAddr, "<ip>", 0, "IP address of a push server (default: 0)", 2},
    {"net-push-port", OptNetPushPort, "<port>", 0, "TCP port of the push server (default: 0)", 2},
    {"net-push-raw", OptNetPushRaw, 0, 0, "Push server forward raw data (default)", 2},
    {"net-push-beast", OptNetPushBeast, 0, 0, "Push server forward beast data", 2},
    {"net-push-sbs", OptNetPushSbs, 0, 0, "Push server forward basestation data", 2},
    {"net-push-delay", OptNetPushDelay, "<seconds>", 0, "Push server re-connection delay (default: 30)", 2},
    {"net-heartbeat", OptNetHeartbeat, "<rate>", 0, "TCP heartbeat rate in seconds (default: 60 sec; 0 to disable)", 2},
    {"net-buffer", OptNetBuffer, "<n>", 0, "TCP buffer size 64Kb * (2^n) (default: n=2, 256Kb)", 2},
    {"net-verbatim", OptNetVerbatim, 0, 0, "Forward messages unchanged", 2},
    #ifdef ENABLE_RTLSDR
        {0,0,0,0, "RTL-SDR options:", 3},
        {0,0,0, OPTION_DOC, "use with --device-type rtlsdr", 3},
        {"device", OptDevice, "<index|serial>", 0, "Select device by index or serial number", 3},
        {"enable-agc", OptRtlSdrEnableAgc, 0, 0, "Enable digital AGC (not tuner AGC!)", 3},
        {"ppm", OptRtlSdrPpm, "<correction>", 0, "Set oscillator frequency correction in PPM", 3},
    #endif
    #ifdef ENABLE_BLADERF
        {0,0,0,0, "BladeRF options:", 4},
        {0,0,0, OPTION_DOC, "use with --device-type bladerf", 4},
        {"device", OptDevice, "<ident>",  0, "Select device by bladeRF 'device identifier'", 4},
        {"bladerf-fpga",            1001, "<path>",   0, "Use alternative FPGA bitstream ('' to disable FPGA load)", 4},
        {"bladerf-decimation",      1002, "<N>",      0, "Assume FPGA decimates by a factor of N", 4},
        {"bladerf-bandwidth",       1003, "<hz>",     0, "Set LPF bandwidth ('bypass' to bypass the LPF)", 4},
    #endif
    {0,0,0,0, "Modes-S Beast options:", 5},
    {0,0,0, OPTION_DOC, "use with --device-type modesbeast", 5},
    {0,0,0, OPTION_DOC, "Beast binary protocol and hardware handshake are always enabled.", 5},
    {"beast-serial", OptBeastSerial, "<path>", 0, "Path to Beast serial device (default /dev/ttyUSB0)", 5},
    {"beast-df1117-on", OptBeastDF1117, 0, 0, "Turn ON DF11/17-only filter", 5},
    {"beast-mlat-off", OptBeastMlatTimeOff, 0, 0, "Turn OFF MLAT time stamps", 5},
    {"beast-crc-off", OptBeastCrcOff, 0, 0, "Turn OFF CRC checking", 5},
    {"beast-df045-on", OptBeastDF045, 0, 0, "Turn ON DF0/4/5 filter", 5},
    {"beast-fec-off", OptBeastFecOff, 0, 0, "Turn OFF forward error correction", 5},
    {"beast-modeac", OptBeastModeAc, 0, 0, "Turn ON mode A/C", 5},

    {0,0,0,0, "GNS5894 options:", 6},
    {0,0,0, OPTION_DOC, "use with --device-type gns5894", 6},
    {0,0,0, OPTION_DOC, "Expects ASCII HEX protocal input.", 6},
    {"beast-serial", OptBeastSerial, "<path>", 0, "Path to GNS5894 serial device (default /dev/ttyAMA0)", 6},

    {0,0,0,0, "ifile-specific options:", 7},
    {0,0,0, OPTION_DOC, "use with --ifile", 7},
    {"ifile", OptIfileName, "<path>", 0, "Read samples from given file ('-' for stdin)", 7},
    {"iformat", OptIfileFormat, "<type>", 0, "Set sample format (UC8, SC16, SC16Q11)", 7},
    {"throttle", OptIfileThrottle, 0, 0, "Process samples at the original capture speed", 7},
    #ifdef ENABLE_PLUTOSDR
        {0,0,0,0, "ADALM-Pluto SDR options:", 8},
        {0,0,0, OPTION_DOC, "use with --device-type plutosdr", 8},
        {"pluto-uri", OptPlutoUri, "<USB uri>", 0, "Create USB context from this URI.(eg. usb:1.2.5)", 8},
        {"pluto-network", OptPlutoNetwork, "<hostname or IP>", 0, "Hostname or IP to create networks context. (default pluto.local)", 8},
    #endif
#endif
    {0,0,0,0, "Help options:", 100},
    { 0 }
};

#endif /* HELP_H */
