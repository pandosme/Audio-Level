#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <math.h>
#include "cJSON.h"
#include "ACAP.h"
#include "MQTT.h"
#include "PipeLevel.h"

#define APP_PACKAGE	"pipelevel"

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
//#define LOG_TRACE(fmt, args...)    {}

int lastInterval = 2;
int lastState = 0;

static void peak_callback(unsigned int n_channels, const float *peaks, float ambient_dBFS, void *userdata) {
    static int lastState = 0; // persistent per-sensor

    cJSON* settings = ACAP_Get_Config("settings");
    if(!settings) return;
    cJSON* level = cJSON_GetObjectItem(settings,"level");
    if(!level) return;

    int is_dynamic = 1;
    cJSON* dyn_item = cJSON_GetObjectItem(level, "dynamic");
    if (dyn_item && (dyn_item->type == cJSON_True || (dyn_item->type == cJSON_Number && dyn_item->valueint)))
        is_dynamic = 1;
    else if (dyn_item && (dyn_item->type == cJSON_False || (dyn_item->type == cJSON_Number && dyn_item->valueint == 0)))
        is_dynamic = 0;

    float dBFS = (peaks[0] > 0.000001f) ? 20.0f * log10f(peaks[0]) : -96.0f;
    int state = lastState;

    if (is_dynamic) {
        double high = cJSON_GetObjectItem(level,"dynamicAlert") ? cJSON_GetObjectItem(level,"dynamicAlert")->valuedouble : 6;
        double normal = cJSON_GetObjectItem(level,"dynamicNormal") ? cJSON_GetObjectItem(level,"dynamicNormal")->valuedouble : 3;

        if (lastState == 0 && dBFS > ambient_dBFS + high) state = 1;
        if (lastState == 1 && dBFS < ambient_dBFS + normal) state = 0;
    } else {
        double fixed_high = cJSON_GetObjectItem(level,"fixedAlert") ? cJSON_GetObjectItem(level,"fixedAlert")->valuedouble : -16;
        double fixed_normal = cJSON_GetObjectItem(level,"fixedNormal") ? cJSON_GetObjectItem(level,"fixedNormal")->valuedouble : -20;

        if (lastState == 0 && dBFS > fixed_high) state = 1;
        if (lastState == 1 && dBFS < fixed_normal) state = 0;
    }

    if(state != lastState) {
        lastState = state;
        ACAP_EVENTS_Fire_State("alert", state);
    }

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "state", state ? 1 : 0);
    cJSON_AddNumberToObject(payload, "level", dBFS);
    cJSON_AddNumberToObject(payload, "ambient", ambient_dBFS);
    char topic[64];
    snprintf(topic, sizeof(topic), "level/%s", ACAP_DEVICE_Prop("serial"));
    MQTT_Publish_JSON(topic, payload, 0, 0);
    cJSON_Delete(payload);
}


void
Settings_Updated_Callback( const char* service, cJSON* data) {
	char* json = cJSON_PrintUnformatted(data);
	LOG_TRACE("%s: Service=%s Data=%s\n",__func__, service, json);
	free(json);
	
	if( strcmp(service,"settings") == 0 ) {
		cJSON* level = cJSON_GetObjectItem(data,"level");
		if( !level )
			return;
		int interval = cJSON_GetObjectItem(level,"interval")?cJSON_GetObjectItem(level,"interval")->valueint:2;
		if( interval < 1 )
			interval = 1;
		if( interval > 15 )
			interval = 15;
		if( interval != lastInterval ) {
			lastInterval = interval;
			PipeLevel_set_interval(interval);
		}
	}
	if( strcmp(service,"level") == 0 ) {
		int interval = cJSON_GetObjectItem(data,"interval")?cJSON_GetObjectItem(data,"interval")->valueint:2;
		if( interval < 1 )
			interval = 1;
		if( interval > 15 )
			interval = 15;
		if( interval != lastInterval ) {
			lastInterval = interval;
			PipeLevel_set_interval(interval);
		}
	}
}


void
Main_MQTT_Status(int state) {
    char topic[64];
    cJSON* message = 0;

    switch (state) {
        case MQTT_INITIALIZING:
            LOG("%s: Initializing\n", __func__);
            break;
        case MQTT_CONNECTING:
            LOG("%s: Connecting\n", __func__);
            break;
        case MQTT_CONNECTED:
            LOG("%s: Connected\n", __func__);
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddTrueToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_DISCONNECTING:
            sprintf(topic, "connect/%s", ACAP_DEVICE_Prop("serial"));
            message = cJSON_CreateObject();
            cJSON_AddFalseToObject(message, "connected");
            cJSON_AddStringToObject(message, "address", ACAP_DEVICE_Prop("IPv4"));
            MQTT_Publish_JSON(topic, message, 0, 1);
            cJSON_Delete(message);
            break;
        case MQTT_RECONNECTED:
            LOG("%s: Reconnected\n", __func__);
            break;
        case MQTT_DISCONNECTED:
            LOG("%s: Disconnect\n", __func__);
            break;
    }
}

void
Main_MQTT_Subscription_Message(const char *topic, const char *payload) {
    LOG("Message arrived: %s %s\n", topic, payload);
}


static GMainLoop *main_loop = NULL;

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

int
main(void) {
	openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);
	LOG("------ Starting ACAP Service ------\n");
	
	ACAP( APP_PACKAGE, Settings_Updated_Callback );

    MQTT_Init(Main_MQTT_Status, Main_MQTT_Subscription_Message);
    ACAP_Set_Config("mqtt", MQTT_Settings());	
	MQTT_Subscribe( "my_topic" );

	ACAP_EVENTS_Add_Event( "alert", "PipeLevel: Alert", 1 );
    PipeLevel_init(peak_callback, NULL);
	
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed");
	}

	g_main_loop_run(main_loop);
	LOG("Terminating and cleaning up %s\n",APP_PACKAGE);
    Main_MQTT_Status(MQTT_DISCONNECTING);
    MQTT_Cleanup();
	ACAP_Cleanup();
	
    return 0;
}
