#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <gio/gio.h>

using namespace std;

extern "C" {
int ledInit(void);
int ledShow(int num, int times, int speed,
	int time, int style, int mute_led, int listen_led);
int ledRelease(void);
};

int ledInit(void)
{
//    PropertyConfigurator::configure("/usr/bin/3r_logging.properties");
    /*
    TRACE();
    DBG();
    INFO();
    WARN();
    ERROR();
    FATAL();
    */

	return 0;
}

static int last_style = -1;

static GDBusConnection* dbusConnectionInstance()
{
	static GDBusConnection *g_connection = NULL;
	if (NULL == g_connection) {
		GError *error = NULL;
		g_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	}

	return g_connection;
}

bool stop_by_chinese_service = false;
int ledShow(int num, int times, int speed,
    int time, int style, int mute_led, int listen_led)
{
	bool to_idle = false;

	//	INFO("before new led command. style: %d, to_idle: %d, last_style: %d\n", style, to_idle, last_style);
	g_print("------------------before new led command. style: %d, to_idle: %d, last_style: %d-------\n", style, to_idle, last_style);
	GError* error = NULL;
	GVariant *v = NULL;
	GVariant *v_b = NULL;
	GVariant **v_tuple = NULL;
	const int n = 2;
	const gchar ** new_animations = (const gchar * * ) g_new0(gchar *, n);

//	g_print("L%d.\n", __LINE__);

	if (style == 15) { // show_idle()
		if (29 == last_style) { // last_style is show_microphone_off()
			goto out;
		} else if (28 == last_style
			|| (last_style >= 16 && last_style <= 21 || 14 == last_style)) { // last style is LISTENING THINKING or SPEAKING
			if (stop_by_chinese_service == false) {
			    new_animations[0] = "/usr/local/Alexa/animation/single/active-ending.animation";
                        }
		} else {
			to_idle = true;
			last_style = -1;
//			goto out;
		}
	} else if ((style >= -37) && (style <= 13)) { // volume change
		new_animations[0] = "/usr/local/Alexa/animation/single/volume-changed.animation";
	} else if (style == 14) { // show_thinking()
		new_animations[0] = "/usr/local/Alexa/animation/single/active-thinking.animation";
	} else if (style == 28) { // show_speaking()
		new_animations[0] = "/usr/local/Alexa/animation/single/active-talking.animation";
/*		
	} else if (style == 29) { // show_microphone_off()
		new_animations = vector<string>({
			"/usr/local/Alexa/animation/single/mics-off_start.animation",
			"/usr/local/Alexa/animation/single/mics-off_on.animation"
			});
	} else if (style == 30) {  // show_microphone_on()
		new_animations = vector<string>({ "/usr/local/Alexa/animation/single/mics-off_end.animation" });
*/
	} else if (style >= 16 && style <= 21) { // show_listening()
		new_animations[0] = "/usr/local/Alexa/animation/single/active-waking.animation";
	} else if (style == 32) { // printDoNotDisturbScreen()
		new_animations[0] = "/usr/local/Alexa/animation/single/do_not_disturb.animation";
	} else if (style == 33) { // printWifiDisconnectedError()
		goto out; // Network state no longer changes the LED ring.
	} else if (style == 34) { // printNotificationIncoming()
		new_animations[0] = "/usr/local/Alexa/animation/single/ntf_incoming.animation";
	} else if (style == 35) { // printAlertLed()
		new_animations[0] = "/usr/local/Alexa/animation/single/alert.animation";
	} else if (style == 36) { // printAvsNotAuthorized()
		new_animations[0] = "/usr/local/Alexa/animation/single/avs_not_authorized.animation";
	} else {
		goto out;
	}
	last_style = style;

//	INFO("after new led command. style: %d, to_idle: %d, last_style: %d, new_animations.size(): %d\n", style, to_idle, last_style, new_animations.size());
//	for (int i = 0; i < new_animations.size(); i++) {
//		INFO("new_animation[%d]: %s\n", i, new_animations[i].c_str());
//	}

	if (new_animations[0] != NULL)
		g_print("L%d. new_animations[0]: %s\n", __LINE__, new_animations[0]);

//	TRACE();
	v_tuple = g_new(GVariant *, 2);
	v_tuple[0] = g_variant_new_boolean(to_idle ? TRUE: FALSE);
	v_tuple[1] = g_variant_new_strv((const gchar * const *) new_animations, -1);
	v = g_variant_new_tuple(v_tuple, 2);
	g_free(v_tuple);

	g_print("L%d. v: %s\n", __LINE__, g_variant_print(v, TRUE));

	g_dbus_connection_emit_signal (dbusConnectionInstance(),
								   NULL, /* destination bus name */
								   "/com/3r/EventBus",
								   "com._3reality.EventBus",
								   "LedShow",
								   v,
								   &error);

out:
	g_free(new_animations);
	stop_by_chinese_service = false;

	return 0;
}

int ledRelease(void)
{
//	led_released = true;
//
//	if (led_thread.get_id() != thread::id()
//		&& led_thread.joinable())
//		led_thread.join();
//
//	turn_off_all();

	return 0;
}
