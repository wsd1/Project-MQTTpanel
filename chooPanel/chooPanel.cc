extern "C"
{
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <err.h>
#include <dirent.h>
#include <time.h>
}

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "led-matrix.h"
#include "graphics.h"

#include "json.h"
#include "json_util.h"
#include "mosquitto.h"

#define mqtt_host "192.168.31.79"
#define mqtt_port 1883

int sflags = 0;
#define json_object_to_json_string(obj) json_object_to_json_string_ext(obj,sflags)


using rgb_matrix::Canvas;
using rgb_matrix::Color;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;



// ################################################################################

struct mqttCfg
{
	const char *clientid;
	const char *host;
	uint16_t port;
} ;

typedef int64_t tmillis_t;

// ################################################################################

json_object *ConfigJSON;

struct mosquitto *mosq;
RGBMatrix::Options panelOptions;

RGBMatrix *canvas;
FrameCanvas *offscreen_canvas;

struct mqttCfg glbMqttCfg;
volatile bool Interrupt = false;

// ################################################################################

void spinning(){
	char spin[5] = "\\|/-";
	static int spin_idx = 0;
	printf("\r%c", spin[spin_idx++]);
	fflush(stdout);
	spin_idx = spin_idx % 4;
}

static tmillis_t GetTimeInMillis()
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}


static void SleepMillis(tmillis_t milli_seconds)
{
	if (milli_seconds <= 0) return;
	struct timespec ts;
	ts.tv_sec = milli_seconds / 1000;
	ts.tv_nsec = (milli_seconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}


static void InterruptHandler(int signo)
{
	Interrupt = true;
}

static int usage(const char *progname)
{
	fprintf(stderr, "usage: %s [options] <image> [option] [<image> ...]\n", progname);

	fprintf(stderr, "Options:\n"
		"\t-O<streamfile>            : Output to stream-file instead of matrix (Don't need to be root).\n"
		"\t-C                        : Center images.\n"

		"\nThese options affect images following them on the command line:\n"
		"\t-w<seconds>               : Regular image: "
		"Wait time in seconds before next image is shown (default: 1.5).\n"
		"\t-t<seconds>               : "
		"For animations: stop after this time.\n"
		"\t-l<loop-count>            : "
		"For animations: number of loops through a full cycle.\n"
		"\t-D<animation-delay-ms>    : "
		"For animations: override the delay between frames given in the\n"
		"\t                            gif/stream animation with this value. Use -1 to use default value.\n"

		"\nOptions affecting display of multiple images:\n"
		"\t-f                        : "
		"Forever cycle through the list of files on the command line.\n"
		"\t-s                        : If multiple images are given: shuffle.\n"
		"\nDisplay Options:\n"
		"\t-V<vsync-multiple>        : Expert: Only do frame vsync-swaps on multiples of refresh (default: 1)\n"
		"\t-L                        : Large display, in which each chain is 'folded down'\n"
		"\t                            in the middle in an U-arrangement to get more vertical space.\n"
		"\t-R<angle>                 : Rotate output; steps of 90 degrees\n"
		);

	fprintf(stderr, "\nGeneral LED matrix options:\n");
	rgb_matrix::PrintMatrixFlags(stderr);

	fprintf(stderr,
		"\nSwitch time between files: "
		"-w for static images; -t/-l for animations\n"
		"Animated gifs: If both -l and -t are given, "
		"whatever finishes first determines duration.\n");

	fprintf(stderr, "\nThe -w, -t and -l options apply to the following images "
		"until a new instance of one of these options is seen.\n"
		"So you can choose different durations for different images.\n");

	return(1);
}

// ################################################################################


// 在 ConfigJSON.panel对象中检查Key，不存在则用 panelOptions[key] 值创建；
// 如果 JSONpref == true，那么 panelOptions[key] 设置为json中的值；
void setPanelConfig(char *Key, bool Value, bool JSONpref)
{
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;



	//上面在全局结构变量 panelOptions 中找到的相应key 和 value值
	bool *ptrKey;
	if (strcmp(Key, "show_refresh_rate") == 0)
		ptrKey = &panelOptions.show_refresh_rate;

	else if (strcmp(Key, "inverse_colors") == 0)
		ptrKey = &panelOptions.inverse_colors;

	else
		return;


	//json结构panel对象下面，如果没有就创建（值继承panelOptions.key）
	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))	
	{
		keyObject = json_object_new_boolean((json_bool)(*ptrKey));
		json_object_object_add(rootObject, Key, keyObject);
	}


	//函数参数JSONpref 若为true，则 panelOptions.key 获取json对象中相应key值
	if (JSONpref == true)
		(*ptrKey) = (bool *) json_object_get_boolean(keyObject);
	else
		(*ptrKey) = Value;
}


void setPanelConfig(char *Key, char *Value, bool JSONpref)
{
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;


	char *ptrKey;
	if (strcmp(Key, "hardware_mapping") == 0)
		ptrKey = (char *)panelOptions.hardware_mapping;

	else if (strcmp(Key, "led_rgb_sequence") == 0)
		ptrKey = (char *)panelOptions.led_rgb_sequence;

	else
		return;


	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))
	{
		keyObject = json_object_new_string(ptrKey);
		json_object_object_add(rootObject, Key, keyObject);
	}
	if (JSONpref == true)
		strcpy((char *)json_object_get_string(keyObject), ptrKey);
	else
		strcpy(Value, ptrKey);
}


void setPanelConfig(char *Key, int Value, bool JSONpref)
{
	struct json_object *rootObject;
	if (!json_object_object_get_ex(ConfigJSON, "panel", &rootObject))
		return;


	int *ptrKey;
	if (strcmp(Key, "rows") == 0)
		ptrKey = &panelOptions.rows;

	else if (strcmp(Key, "cols") == 0)
		ptrKey = &panelOptions.cols;

	else if (strcmp(Key, "chain_length") == 0)
		ptrKey = &panelOptions.chain_length;

	else if (strcmp(Key, "parallel") == 0)
		ptrKey = &panelOptions.parallel;

	else if (strcmp(Key, "multiplexing") == 0)
		ptrKey = &panelOptions.multiplexing;

	else if (strcmp(Key, "brightness") == 0)
		ptrKey = &panelOptions.brightness;

	else if (strcmp(Key, "scan_mode") == 0)
		ptrKey = &panelOptions.scan_mode;

	else if (strcmp(Key, "pwm_bits") == 0)
		ptrKey = &panelOptions.pwm_bits;

	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		ptrKey = &panelOptions.pwm_lsb_nanoseconds;

	else if (strcmp(Key, "row_address_type") == 0)
		ptrKey = &panelOptions.row_address_type;
	else
		return;


	struct json_object *keyObject;
	if (!json_object_object_get_ex(rootObject, Key, &keyObject))
	{
		keyObject = json_object_new_int((int32_t)(*ptrKey));
		json_object_object_add(rootObject, Key, keyObject);
	}
	if (JSONpref == true)
		(*ptrKey) = json_object_get_int(keyObject);
	else
		(*ptrKey) = Value;
}




// ConfigJSON.panel[key] => panelOptions[key] 
// JSON => struct
void setPanelConfig(char *Key)
{	
	if (strcmp(Key, "show_refresh_rate") == 0)
		setPanelConfig(Key, true, true);

	else if (strcmp(Key, "inverse_colors") == 0)
		setPanelConfig(Key, true, true);

	else if (strcmp(Key, "hardware_mapping") == 0)
		setPanelConfig(Key, (char *)NULL, true);

	else if (strcmp(Key, "led_rgb_sequence") == 0)
		setPanelConfig(Key, (char *)NULL, true);

	else if (strcmp(Key, "rows") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "cols") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "chain_length") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "parallel") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "multiplexing") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "brightness") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "scan_mode") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "pwm_bits") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "pwm_lsb_nanoseconds") == 0)
		setPanelConfig(Key, (int)0, true);

	else if (strcmp(Key, "row_address_type") == 0)
		setPanelConfig(Key, (int)0, true);

}


// ################################################################################
int initPanel(int argc, char *argv[])
{

	// ConfigJSON.panel[key] => panelOptions[key] 
	setPanelConfig((char *) "hardware_mapping");
	setPanelConfig((char *) "led_rgb_sequence");
	setPanelConfig((char *) "rows");
	setPanelConfig((char *) "cols");
	setPanelConfig((char *) "chain_length");
	setPanelConfig((char *) "parallel");
	setPanelConfig((char *) "multiplexing");
	setPanelConfig((char *) "brightness");
	setPanelConfig((char *) "scan_mode");
	setPanelConfig((char *) "pwm_bits");
	setPanelConfig((char *) "pwm_lsb_nanoseconds");
	setPanelConfig((char *) "row_address_type");
	setPanelConfig((char *) "show_refresh_rate");
	setPanelConfig((char *) "inverse_colors");

	struct json_object *rootObject;

	//从JSON中读取其他配置 ConfigJSON.mqtt[] => glbMqttCfg[] 
	if (json_object_object_get_ex(ConfigJSON, "mqtt", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "clientid", &keyObject))
		{
			keyObject = json_object_new_string("defaultID:chooPanel");
			json_object_object_add(rootObject, "clientid", keyObject);
		}
		glbMqttCfg.clientid = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "host", &keyObject))
		{
			keyObject = json_object_new_string("mqtt.eclipseprojects.io");	//paho.mqtt.c demo broker
			json_object_object_add(rootObject, "host", keyObject);
		}
		glbMqttCfg.host = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "port", &keyObject))
		{
			keyObject = json_object_new_int(1883);
			json_object_object_add(rootObject, "port", keyObject);
		}
		glbMqttCfg.port = json_object_get_int(keyObject);

	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));



	/*
	//从JSON中读取其他配置 ConfigJSON.background[] => displayImage[] 
	if (json_object_object_get_ex(ConfigJSON, "background", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "image", &keyObject))
		{
			keyObject = json_object_new_string("Alien.gif");
			json_object_object_add(rootObject, "image", keyObject);
		}
		displayImage.image = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("images/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayImage.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayImage.show = json_object_get_boolean(keyObject);
	}
	// printf("JSON(background):%s\n", json_object_get_string(rootObject));

	//从JSON中读取其他配置 ConfigJSON.time[] => displayTime[] 
	if (json_object_object_get_ex(ConfigJSON, "time", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("%H:%M");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayTime.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayTime.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("8x13.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayTime.fontname = json_object_get_string(keyObject);
		fetchFont(&displayTime);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(8);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayTime.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(10);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayTime.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayTime.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("FF00FF");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayTime.red, &displayTime.green, &displayTime.blue);

		if (!json_object_object_get_ex(rootObject, "timezone", &keyObject))
		{
			keyObject = json_object_new_string(getenv("TZ"));
			json_object_object_add(rootObject, "timezone", keyObject);
		}
		displayTime.timezone = json_object_get_string(keyObject);
	}
	// printf("JSON(time):%s\n", json_object_get_string(rootObject));

	//从JSON中读取其他配置 ConfigJSON.date[] => displayDate[] 
	if (json_object_object_get_ex(ConfigJSON, "date", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("%Y/%m/%d");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayDate.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayDate.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("6x9.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayDate.fontname = json_object_get_string(keyObject);
		fetchFont(&displayDate);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(8);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayDate.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(30);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayDate.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(1);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayDate.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("FF00FF");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayDate.red, &displayDate.green, &displayDate.blue);

		if (!json_object_object_get_ex(rootObject, "timezone", &keyObject))
		{
			keyObject = json_object_new_string(getenv("TZ"));
			json_object_object_add(rootObject, "timezone", keyObject);
		}
		displayDate.timezone = json_object_get_string(keyObject);
	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));


	//从JSON中读取其他配置 ConfigJSON.text[] => displayText[] 
	if (json_object_object_get_ex(ConfigJSON, "text", &rootObject))
	{
		struct json_object *keyObject;

		if (!json_object_object_get_ex(rootObject, "format", &keyObject))
		{
			keyObject = json_object_new_string("");
			json_object_object_add(rootObject, "format", keyObject);
		}
		displayText.format = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "dir", &keyObject))
		{
			keyObject = json_object_new_string("/usr/local/share/fonts/rgbmatrix/");
			json_object_object_add(rootObject, "dir", keyObject);
		}
		displayText.dir = json_object_get_string(keyObject);

		if (!json_object_object_get_ex(rootObject, "font", &keyObject))
		{
			keyObject = json_object_new_string("6x9.bdf");
			json_object_object_add(rootObject, "font", keyObject);
		}
		displayText.fontname = json_object_get_string(keyObject);
		fetchFont(&displayText);

		if (!json_object_object_get_ex(rootObject, "x", &keyObject))
		{
			keyObject = json_object_new_int(0);
			json_object_object_add(rootObject, "x", keyObject);
		}
		displayText.x = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "y", &keyObject))
		{
			keyObject = json_object_new_int(30);
			json_object_object_add(rootObject, "y", keyObject);
		}
		displayText.y = json_object_get_int(keyObject);

		if (!json_object_object_get_ex(rootObject, "show", &keyObject))
		{
			keyObject = json_object_new_boolean(0);
			json_object_object_add(rootObject, "show", keyObject);
		}
		displayText.show = json_object_get_boolean(keyObject);

		if (!json_object_object_get_ex(rootObject, "color", &keyObject))
		{
			keyObject = json_object_new_string("888888");
			json_object_object_add(rootObject, "color", keyObject);
		}
		convRGBstr((char *)json_object_get_string(keyObject), &displayText.red, &displayText.green, &displayText.blue);

		displayText.timezone = "";	// Not used for plain text.
	}
	// printf("JSON(date):%s\n", json_object_get_string(rootObject));
	*/


	rgb_matrix::RuntimeOptions runtime_opt;
	if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &panelOptions, &runtime_opt))
	{
		return usage(argv[0]);
	}

	// Prepare matrix
	canvas = CreateMatrixFromOptions(panelOptions, runtime_opt);
	if (canvas == NULL)
		return 1;

	offscreen_canvas = canvas->CreateFrameCanvas();
	printf("Panel size: %dx%d. Hardware gpio mapping: %s\n", canvas->width(), canvas->height(), panelOptions.hardware_mapping);

	return 0;
}

// ################################################################################



void connect_callback(struct mosquitto *mosq, void *obj, int result)
{
	printf("Connect_callback(), rc=%d --------------->\n", result);

	printf("mosquitto_subscribing...\n");
	mosquitto_subscribe(mosq, NULL, "/chooPanel/#", 0);
	printf("mosquitto_subscribe issued...\n");

}


void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	//bool match = false;
	int payloadInt = 0x00;
	// int payloadInt = std::stoi((char *)message->payload, nullptr, 0);
	if (message->payloadlen > 0)
		payloadInt = strtol((char *)message->payload, nullptr, 0);

	printf("got message '%.*s' for topic '%s'\n", message->payloadlen, (char*) message->payload, message->topic);

	/*
	// Background image topics.
	mosquitto_topic_matches_sub("/display/panel/#", message->topic, &match);
	if (match)
	{
		mosquitto_topic_matches_sub("/display/panel/brightness", message->topic, &match);
		if (match)
		{
			setPanelConfig((char *)"brightness", (char *)message->payload, false);
			printf("Set brightness: '%s'\n", (char *)message->payload);
		}
	}

	mosquitto_topic_matches_sub("/display/background/load", message->topic, &match);
	if (match)
	{
		displayImage.image = (char *)message->payload;
		LoadImage(displayImage.image);
	}


	// Time topics.
	mosquitto_topic_matches_sub("/display/time/format", message->topic, &match);
	if (match)
		displayTime.format = (char *)message->payload;

	mosquitto_topic_matches_sub("/display/time/show", message->topic, &match);
	if (match)
		displayTime.show = payloadInt;

	mosquitto_topic_matches_sub("/display/time/x", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
		displayTime.x = payloadInt;

	mosquitto_topic_matches_sub("/display/time/y", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
		displayTime.y = payloadInt;

	mosquitto_topic_matches_sub("/display/time/dir", message->topic, &match);
	if (match)
		displayTime.dir = (char *)message->payload;

	mosquitto_topic_matches_sub("/display/time/font", message->topic, &match);
	if (match)
	{
		displayTime.fontname = (char *)message->payload;
		fetchFont(&displayTime);
	}

	mosquitto_topic_matches_sub("/display/time/color", message->topic, &match);
	if (match)
		convRGBstr((char *)message->payload, &displayTime.red, &displayTime.green, &displayTime.blue);

	mosquitto_topic_matches_sub("/display/time/zone", message->topic, &match);
	if (match)
		strcpy((char *)displayTime.timezone, (char *)message->payload);


	// Date topics.
	mosquitto_topic_matches_sub("/display/date/format", message->topic, &match);
	if (match)
		displayDate.format = (char *)message->payload;

	mosquitto_topic_matches_sub("/display/date/show", message->topic, &match);
	if (match)
		displayDate.show = payloadInt;

	mosquitto_topic_matches_sub("/display/date/x", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
		displayDate.x = payloadInt;

	mosquitto_topic_matches_sub("/display/date/y", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
		displayDate.y = payloadInt;

	mosquitto_topic_matches_sub("/display/date/dir", message->topic, &match);
	if (match)
		displayDate.dir = (char *)message->payload;

	mosquitto_topic_matches_sub("/display/date/font", message->topic, &match);
	if (match)
	{
		displayDate.fontname = (char *)message->payload;
		fetchFont(&displayDate);
	}

	mosquitto_topic_matches_sub("/display/date/color", message->topic, &match);
	if (match)
		convRGBstr((char *)message->payload, &displayDate.red, &displayDate.green, &displayDate.blue);

	mosquitto_topic_matches_sub("/display/date/zone", message->topic, &match);
	if (match)
		strcpy((char *)displayDate.timezone, (char *)message->payload);


	// Text topics.
	mosquitto_topic_matches_sub("/display/text/format", message->topic, &match);
	if (match)
	{
		if (message->payloadlen == 0x00)
			displayText.show = false;
		else
		{
			displayText.show = true;
			strcpy((char *)displayText.format, (char *)message->payload);
		}
	}

	mosquitto_topic_matches_sub("/display/text/show", message->topic, &match);
	if (match)
		displayText.show = payloadInt;

	mosquitto_topic_matches_sub("/display/text/x", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
	{
		printf("Int:%d\n", payloadInt);
		displayText.x = payloadInt;
	}

	mosquitto_topic_matches_sub("/display/text/y", message->topic, &match);
	if (match && (payloadInt >= 0) && (payloadInt < canvas->width()))
		displayText.y = payloadInt;

	mosquitto_topic_matches_sub("/display/text/dir", message->topic, &match);
	if (match)
		displayText.dir = (char *)message->payload;

	mosquitto_topic_matches_sub("/display/text/font", message->topic, &match);
	if (match)
	{
		displayText.fontname = (char *)message->payload;
		fetchFont(&displayText);
	}

	mosquitto_topic_matches_sub("/display/text/color", message->topic, &match);
	if (match)
		convRGBstr((char *)message->payload, &displayText.red, &displayText.green, &displayText.blue);


	*/
}

int initMQTT(void)
{
	// uint8_t reconnect = true;
	//char clientid[24];
	int rc = 0;

	//const tmillis_t old = GetTimeInMillis();

	mosquitto_lib_init();

	//memset(clientid, 0, 24);
	//snprintf(clientid, 23, "mysql_log_%d", getpid());

	mosq = mosquitto_new(glbMqttCfg.clientid, true, 0);
	if(mosq)
	{
		mosquitto_connect_callback_set(mosq, connect_callback);
		mosquitto_message_callback_set(mosq, message_callback);

		printf("mosquitto_connecting...\n");
		rc = mosquitto_connect(mosq, glbMqttCfg.host, glbMqttCfg.port, 60);
		//printf("mosquitto_connect issued...cost %dms\n", (int)(GetTimeInMillis()-old));
	}

	return(rc);
}


int runMQTT(void)
{
	int rc = 0;
	tmillis_t old = GetTimeInMillis();

	// MQTT client.
	if(mosq)
	{
		//rc = mosquitto_loop(mosq, -1, 1);
		rc = mosquitto_loop(mosq, 0, 1);
		//printf("mosquitto_loop() cost %dms\n", (int)(GetTimeInMillis()-old));
		if(!Interrupt && rc)
		{
			switch (rc){
				case MOSQ_ERR_SUCCESS:
					printf("mosquitto_loop ret: MOSQ_ERR_SUCCESS\n");
					break;
				case MOSQ_ERR_INVAL:
					printf("mosquitto_loop ret: MOSQ_ERR_INVAL\n");
					break;
				case MOSQ_ERR_NOMEM:
					printf("mosquitto_loop ret: MOSQ_ERR_NOMEM\n");
					break;
				case MOSQ_ERR_NO_CONN:
					printf("mosquitto_loop ret: MOSQ_ERR_NO_CONN\n");
					break;
				case MOSQ_ERR_CONN_LOST:
					printf("mosquitto_loop ret: MOSQ_ERR_CONN_LOST\n");
					break;
				case MOSQ_ERR_PROTOCOL:
					printf("mosquitto_loop ret: MOSQ_ERR_PROTOCOL\n");
					break;
				case MOSQ_ERR_ERRNO:
					printf("mosquitto_loop ret: MOSQ_ERR_ERRNO\n");
					break;
				default :
					printf("mosquitto_loop ret: %d\n", rc);
					
			}


			sleep(10);

			printf("reconnecting...\n");
			old = GetTimeInMillis();
			int rt = mosquitto_reconnect_async(mosq);
			printf("reconnection issued...ret:%d cost %dms\n", rt, (int)(GetTimeInMillis()-old));
		}
	}

	return(rc);
}




int main(int argc, char *argv[])
{
	signal(SIGTERM, InterruptHandler);
	signal(SIGINT, InterruptHandler);

	const char *filename = "config.json";
	int fileJSON = open(filename, O_RDONLY, 0);
	if (fileJSON < 0)
	{
		fprintf(stderr, "FAIL: unable to open %s: %s\n", filename, strerror(errno));
		exit(0);
	}
	close(fileJSON);

	printf("Reading JSON config file '%s'...", filename);
	ConfigJSON = json_object_from_file(filename);
	if (ConfigJSON != NULL)
	{
		printf("Done\n");
		printf("OK: json_object_from_fd(%s)=%s\n", filename, json_object_to_json_string(ConfigJSON));
	}
	else
	{
		fprintf(stderr, "FAIL: unable to parse contents of %s: %s\n", filename, json_object_to_json_string(ConfigJSON));
		exit(1);
	}

	initPanel(argc, argv);
	initMQTT();

	while(!Interrupt)
	{
		//DisplayAnimation(canvas, offscreen_canvas, vsync_multiple);
		runMQTT();
		SleepMillis(500);
		spinning();
	}

	mosquitto_lib_cleanup();

	// Animation finished. Shut down the RGB canvas.
	canvas->Clear();
	delete canvas;

	if (Interrupt)
	{
		fprintf(stderr, "Caught signal. Exiting.\n");
	}
	printf("\nEnded.\n\n");

	return 0;

}

