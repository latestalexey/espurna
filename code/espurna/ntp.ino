/*

NTP MODULE

Copyright (C) 2016-2018 by Xose Pérez <xose dot perez at gmail dot com>

*/

#if NTP_SUPPORT

#include <TimeLib.h>
#include <NtpClientLib.h>
#include <WiFiClient.h>
#include <Ticker.h>

#include "libs/sunrise.h"

unsigned long _ntp_start = 0;
bool _ntp_update = false;
bool _ntp_configure = false;

#if RTC_SUPPORT && RTC_NTP_SYNC_ENA
bool _rtc_update = false;
#endif

extern std::vector<BaseSensor *> _sensors;

// -----------------------------------------------------------------------------
// NTP
// -----------------------------------------------------------------------------

#if WEB_SUPPORT

bool _ntpWebSocketOnReceive(const char * key, JsonVariant& value) {
    return (strncmp(key, "ntp", 3) == 0);
}

void _ntpWebSocketOnSend(JsonObject& root) {
    root["ntpVisible"] = 1;
    root["ntpStatus"] = (timeStatus() == timeSet);
    root["ntpServer"] = getSetting("ntpServer", NTP_SERVER);
    root["ntpOffset"] = getSetting("ntpOffset", NTP_TIME_OFFSET).toInt();
    root["ntpDST"] = getSetting("ntpDST", NTP_DAY_LIGHT).toInt() == 1;
    root["ntpRegion"] = getSetting("ntpRegion", NTP_DST_REGION).toInt();

    root["ntpLatitude"] = getSetting("ntpLatitude", NTP_LATITUDE).toFloat();
    root["ntpLongitude"] = getSetting("ntpLongitude", NTP_LONGITUDE).toFloat();

    if (ntpSynced()) { root["now"] = now();  }
}

#endif

void _ntpStart() {

    _ntp_start = 0;

#if RTC_SUPPORT
    bool cSyncProv = true;
#else
    bool cSyncProv = false;
#endif

    NTP.begin(getSetting("ntpServer", NTP_SERVER),DEFAULT_NTP_TIMEZONE,false,0,NULL,cSyncProv);

#if RTC_SUPPORT
    // set alter sync provider. two attempts for NTP synchro at start occure...
    setSyncProvider(ntp_getTime);
#endif    

    NTP.setInterval(NTP_SYNC_INTERVAL, NTP_UPDATE_INTERVAL);
    NTP.setNTPTimeout(NTP_TIMEOUT);
    _ntpConfigure();

}

void _ntpConfigure() {

    _ntp_configure = false;

    int offset = getSetting("ntpOffset", NTP_TIME_OFFSET).toInt();
    int sign = offset > 0 ? 1 : -1;
    offset = abs(offset);
    int tz_hours = sign * (offset / 60);
    int tz_minutes = sign * (offset % 60);
    if (NTP.getTimeZone() != tz_hours || NTP.getTimeZoneMinutes() != tz_minutes) {
        NTP.setTimeZone(tz_hours, tz_minutes);
        _ntp_update = true;
    }

    bool daylight = getSetting("ntpDST", NTP_DAY_LIGHT).toInt() == 1;
    if (NTP.getDayLight() != daylight) {
        NTP.setDayLight(daylight);
        _ntp_update = true;
    }

    String server = getSetting("ntpServer", NTP_SERVER);
    if (!NTP.getNtpServerName().equals(server)) {
        NTP.setNtpServerName(server);
    }

    uint8_t dst_region = getSetting("ntpRegion", NTP_DST_REGION).toInt();
    NTP.setDSTZone(dst_region);

    // update sunrise provider
    float lat = getSetting("ntpLatitude", NTP_LATITUDE).toFloat();
    float lon = getSetting("ntpLongitude", NTP_LONGITUDE).toFloat();
    DEBUG_MSG_P(PSTR("[NTP] Update Sunrise provider\n[NTP] Time zone : %d\n"), offset);
    DEBUG_MSG_P(PSTR("[NTP] Latitude provider  : %s\n"), String(lat).c_str());
    DEBUG_MSG_P(PSTR("[NTP] Longitude provider  : %s\n"), String(lon).c_str());

    sun.begin(lat,lon,(tz_hours+tz_minutes/60.0f));

    if(ntpSynced()) {
         DEBUG_MSG_P(PSTR("[NTP] Time SYNCED - RESET SENSORS\n"));
         for (unsigned char i=0; i<_sensors.size(); i++) // and reset sunrise sensors
            if(_sensors[i]->getID() == SENSOR_SUNRISE_ID) _sensors[i]->begin();
    }

}

void _ntpUpdate() {

    _ntp_update = false;

    #if WEB_SUPPORT
        wsSend(_ntpWebSocketOnSend);
    #endif

    if (ntpSynced()) {
        time_t t = now();
        #if RTC_SUPPORT && RTC_NTP_SYNC_ENA
            // sync/update rtc here!!!!!!!!!!!!
            if(_rtc_update) setTime_rtc(t);
        #endif

        DEBUG_MSG_P(PSTR("[NTP] UTC Time  : %s\n"), (char *) ntpDateTime(ntpLocal2UTC(t)).c_str());
        DEBUG_MSG_P(PSTR("[NTP] Local Time: %s\n"), (char *) ntpDateTime(t).c_str());
    }

}

void _ntpLoop() {

    if (0 < _ntp_start && _ntp_start < millis()) _ntpStart();
    if (_ntp_configure) _ntpConfigure();
    if (_ntp_update) _ntpUpdate();

    time_t t = now();

        static unsigned char last_minute = 60;
        if (ntpSynced() && (minute() != last_minute)) {
            last_minute = minute();
            #if BROKER_SUPPORT            
                brokerPublish(MQTT_TOPIC_DATETIME, ntpDateTime(t).c_str());
            #endif            
        }
}

void _ntpBackwards() {
    moveSetting("ntpServer1", "ntpServer");
    delSetting("ntpServer2");
    delSetting("ntpServer3");
    int offset = getSetting("ntpOffset", NTP_TIME_OFFSET).toInt();
    if (-30 < offset && offset < 30) {
        offset *= 60;
        setSetting("ntpOffset", offset);
    }
}

// -----------------------------------------------------------------------------

bool ntpSynced() {
    return (year() > 2017);
}

String ntpDateTime(time_t t) {
    char buffer[20];
    snprintf_P(buffer, sizeof(buffer),
        PSTR("%04d-%02d-%02d %02d:%02d:%02d"),
        year(t), month(t), day(t), hour(t), minute(t), second(t)
    );
    return String(buffer);
}

String ntpDateTime() {
    if (ntpSynced()) return ntpDateTime(now());
    return String();
}

time_t ntpLocal2UTC(time_t local) {
    int offset = getSetting("ntpOffset", NTP_TIME_OFFSET).toInt();
    if (NTP.isSummerTime()) offset += 60;
    return local - offset * 60;
}

// -----------------------------------------------------------------------------

void ntpSetup() {

    _ntpBackwards();

    NTP.onNTPSyncEvent([](NTPSyncEvent_t error) {
        if (error) {
            #if WEB_SUPPORT
                wsSend_P(PSTR("{\"ntpStatus\": false}"));
            #endif
            if (error == noResponse) {
                DEBUG_MSG_P(PSTR("[NTP] Error: NTP server not reachable\n"));
            } else if (error == invalidAddress) {
                DEBUG_MSG_P(PSTR("[NTP] Error: Invalid NTP server address\n"));
            }
            _ntp_update = false;
        } else {
            _ntp_update = true;
            #if RTC_SUPPORT && RTC_NTP_SYNC_ENA
                _rtc_update = true;
            #endif

        }
    });

    wifiRegister([](justwifi_messages_t code, char * parameter) {
        if (code == MESSAGE_CONNECTED) _ntp_start = millis() + NTP_START_DELAY;
        #if RTC_SUPPORT
          // system time from local RTC, but still try recovery if enabled (without success)
        else 
            if(code == MESSAGE_ACCESSPOINT_CREATED) _ntp_start = millis() + NTP_START_DELAY;
        #endif                

    });

    #if WEB_SUPPORT
        wsOnSendRegister(_ntpWebSocketOnSend);
        wsOnReceiveRegister(_ntpWebSocketOnReceive);
        wsOnAfterParseRegister([]() { _ntp_configure = true; });
    #endif

    // Register loop
    espurnaRegisterLoop(_ntpLoop);
    
    #if RTC_SUPPORT
        #if TERMINAL_SUPPORT
            _rtcInitCommands();
        #endif
    #endif        

}

#endif // NTP_SUPPORT
