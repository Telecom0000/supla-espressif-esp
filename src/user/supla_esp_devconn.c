/*
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <string.h>
#include <stdio.h>

#include <user_interface.h>
#include <espconn.h>
#include <spi_flash.h>
#include <osapi.h>
#include <mem.h>
#include <stdlib.h>

#include "supla_esp_devconn.h"
#include "supla_esp_cfgmode.h"
#include "supla_esp_gpio.h"
#include "supla_esp_cfg.h"
#include "supla_esp_pwm.h"
#include "supla_ds18b20.h"
#include "supla_dht.h"
#include "supla-dev/srpc.h"
#include "supla-dev/log.h"

#ifdef __FOTA
#include "supla_update.h"
#endif

#define SEND_BUFFER_SIZE 500

typedef struct {

	ETSTimer supla_devconn_timer1;
	ETSTimer supla_iterate_timer;
	ETSTimer supla_watchdog_timer;
	ETSTimer supla_value_timer;

	// ESPCONN_INPROGRESS fix
	char esp_send_buffer[SEND_BUFFER_SIZE];
	int esp_send_buffer_len;
	// ---------------------------------------------

	struct espconn ESPConn;
	esp_tcp ESPTCP;
	ip_addr_t ipaddr;

	void *srpc;
	char registered;

	char recvbuff[RECVBUFF_MAXSIZE];
	unsigned int recvbuff_size;

	char laststate[STATE_MAXSIZE];

	char autoconnect;

	unsigned int last_response;
	int server_activity_timeout;

	uint8 last_wifi_status;


}devconn_params;


static devconn_params *devconn = NULL;

#if defined(RGB_CONTROLLER_CHANNEL) \
    || defined(RGBW_CONTROLLER_CHANNEL) \
    || defined(RGBWW_CONTROLLER_CHANNEL) \
    || defined(DIMMER_CHANNEL)

typedef struct {
    double h;
    double s;
    double v;
} hsv;

typedef struct {

	int counter;

	int ChannelNumber;
	char smoothly;
	char send_value_changed;

	int color;
	int dest_color;

	char color_brightness;
	char dest_color_brightness;

	char brightness;
	char dest_brightness;

	ETSTimer timer;

}devconn_smooth;

devconn_smooth smooth[2];

#endif

#if NOSSL == 1
    #define supla_espconn_sent espconn_sent
    #define supla_espconn_disconnect espconn_disconnect
    #define supla_espconn_connect espconn_connect
#else
    #define supla_espconn_sent espconn_secure_sent
	#define _supla_espconn_disconnect espconn_secure_disconnect
	#define supla_espconn_connect espconn_secure_connect
#endif


void DEVCONN_ICACHE_FLASH supla_esp_devconn_timer1_cb(void *timer_arg);
void DEVCONN_ICACHE_FLASH supla_esp_wifi_check_status(char autoconnect);
void DEVCONN_ICACHE_FLASH supla_esp_srpc_free(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_iterate(void *timer_arg);

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_system_restart(void) {

    if ( supla_esp_cfgmode_started() == 0 ) {

    	os_timer_disarm(&devconn->supla_watchdog_timer);
    	supla_esp_srpc_free();

		#ifdef BOARD_GPIO_BEFORE_REBOOT
		supla_esp_board_before_reboot();
		#endif

		supla_log(LOG_DEBUG, "RESTART");
    	supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

    	system_restart();


    }
}

char DEVCONN_ICACHE_FLASH
supla_esp_devconn_update_started(void) {
#ifdef __FOTA
	return supla_esp_update_started();
#else
	return 0;
#endif
}

#pragma GCC diagnostic pop

int DEVCONN_ICACHE_FLASH
supla_esp_data_read(void *buf, int count, void *dcd) {


	if ( devconn->recvbuff_size > 0 ) {

		count = devconn->recvbuff_size > count ? count : devconn->recvbuff_size;
		os_memcpy(buf, devconn->recvbuff, count);

		if ( count == devconn->recvbuff_size ) {

			devconn->recvbuff_size = 0;

		} else {

			unsigned int a;

			for(a=0;a<devconn->recvbuff_size-count;a++)
				devconn->recvbuff[a] = devconn->recvbuff[count+a];

			devconn->recvbuff_size-=count;
		}

		return count;
	}

	return -1;
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_recv_cb (void *arg, char *pdata, unsigned short len) {

	if ( len == 0 || pdata == NULL )
		return;

	//supla_log(LOG_ERR, "sproto recv %i bytes", len);

	if ( len <= RECVBUFF_MAXSIZE-devconn->recvbuff_size ) {

		os_memcpy(&devconn->recvbuff[devconn->recvbuff_size], pdata, len);
		devconn->recvbuff_size += len;

		supla_esp_devconn_iterate(NULL);

	} else {
		supla_log(LOG_ERR, "Recv buffer size exceeded");
	}

}

int DEVCONN_ICACHE_FLASH
supla_esp_data_write_append_buffer(void *buf, int count) {

	if ( count > 0 ) {

		if ( devconn->esp_send_buffer_len+count > SEND_BUFFER_SIZE ) {

			supla_log(LOG_ERR, "Send buffer size exceeded");
			supla_esp_devconn_system_restart();

			return -1;

		} else {

			memcpy(&devconn->esp_send_buffer[devconn->esp_send_buffer_len], buf, count);
			devconn->esp_send_buffer_len+=count;

			return 0;


		}
	}

	return 0;
}

int DEVCONN_ICACHE_FLASH
supla_esp_data_write(void *buf, int count, void *dcd) {

	int r;

	if ( devconn->esp_send_buffer_len > 0
		 && supla_espconn_sent(&devconn->ESPConn, (unsigned char*)devconn->esp_send_buffer, devconn->esp_send_buffer_len) == 0 ) {

			devconn->esp_send_buffer_len = 0;
	};


	if ( devconn->esp_send_buffer_len > 0 ) {
		return supla_esp_data_write_append_buffer(buf, count);
	}

	if ( count > 0 ) {

		r = supla_espconn_sent(&devconn->ESPConn, buf, count);

		if ( ESPCONN_INPROGRESS == r  ) {
			return supla_esp_data_write_append_buffer(buf, count);
		} else {
			return r == 0 ? count : -1;
		}

	}


	return 0;
}


void DEVCONN_ICACHE_FLASH
supla_esp_set_state(int __pri, const char *message) {

	if ( message == NULL )
		return;

	unsigned char len = strlen(message)+1;

	supla_log(__pri, message);

    if ( len > STATE_MAXSIZE )
    	len = STATE_MAXSIZE;

	os_memcpy(devconn->laststate, message, len);
}

void DEVCONN_ICACHE_FLASH
supla_esp_on_version_error(TSDC_SuplaVersionError *version_error) {

	supla_esp_set_state(LOG_ERR, "Protocol version error");
	supla_esp_devconn_stop();
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_send_channel_values_cb(void *ptr) {

	if ( devconn->srpc != NULL
		 && devconn->registered == 1 ) {

		supla_esp_board_send_channel_values_with_delay(devconn->srpc);

	}

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_send_channel_values_with_delay(void) {

	os_timer_disarm(&devconn->supla_value_timer);
	os_timer_setfn(&devconn->supla_value_timer, (os_timer_func_t *)supla_esp_devconn_send_channel_values_cb, NULL);
	os_timer_arm(&devconn->supla_value_timer, 1500, 0);

}

void DEVCONN_ICACHE_FLASH
supla_esp_on_register_result(TSD_SuplaRegisterDeviceResult *register_device_result) {

	switch(register_device_result->result_code) {
	case SUPLA_RESULTCODE_BAD_CREDENTIALS:
		supla_esp_set_state(LOG_ERR, "Bad credentials!");
		break;

	case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
		supla_esp_set_state(LOG_NOTICE, "Temporarily unavailable!");
		break;

	case SUPLA_RESULTCODE_LOCATION_CONFLICT:
		supla_esp_set_state(LOG_ERR, "Location conflict!");
		break;

	case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
		supla_esp_set_state(LOG_ERR, "Channel conflict!");
		break;
	case SUPLA_RESULTCODE_TRUE:

		supla_esp_gpio_state_connected();

		devconn->server_activity_timeout = register_device_result->activity_timeout;
		devconn->registered = 1;

		supla_esp_set_state(LOG_DEBUG, "Registered and ready.");
		supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

		if ( devconn->server_activity_timeout != ACTIVITY_TIMEOUT ) {

			TDCS_SuplaSetActivityTimeout at;
			at.activity_timeout = ACTIVITY_TIMEOUT;
			srpc_dcs_async_set_activity_timeout(devconn->srpc, &at);

		}

		#ifdef __FOTA
		supla_esp_check_updates(devconn->srpc);
		#endif

		supla_esp_devconn_send_channel_values_with_delay();

		return;

	case SUPLA_RESULTCODE_DEVICE_DISABLED:
		supla_esp_set_state(LOG_NOTICE, "Device is disabled!");
		break;

	case SUPLA_RESULTCODE_LOCATION_DISABLED:
		supla_esp_set_state(LOG_NOTICE, "Location is disabled!");
		break;

	case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
		supla_esp_set_state(LOG_NOTICE, "Device limit exceeded!");
		break;

	case SUPLA_RESULTCODE_GUID_ERROR:
		supla_esp_set_state(LOG_NOTICE, "Incorrect device GUID!");
		break;
	}

	devconn->autoconnect = 0;
	supla_esp_devconn_stop();
}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_set_activity_timeout_result(TSDC_SuplaSetActivityTimeoutResult *result) {
	devconn->server_activity_timeout = result->activity_timeout;
}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_value_changed(int channel_number, char v) {

	if ( devconn->srpc != NULL
		 && devconn->registered == 1 ) {

		//supla_log(LOG_DEBUG, "supla_esp_channel_value_changed(%i, %i)", channel_number, v);

		char value[SUPLA_CHANNELVALUE_SIZE];
		memset(value, 0, SUPLA_CHANNELVALUE_SIZE);
		value[0] = v;

		srpc_ds_async_channel_value_changed(devconn->srpc, channel_number, value);
	}

}

#if defined(RGBW_CONTROLLER_CHANNEL) \
    || defined(RGBWW_CONTROLLER_CHANNEL) \
	|| defined(RGB_CONTROLLER_CHANNEL) \
    || defined(DIMMER_CHANNEL)

void DEVCONN_ICACHE_FLASH
supla_esp_channel_rgbw_to_value(char value[SUPLA_CHANNELVALUE_SIZE], int color, char color_brightness, char brightness) {

	memset(value, 0, SUPLA_CHANNELVALUE_SIZE);

	value[0] = brightness;
	value[1] = color_brightness;
	value[2] = (char)((color & 0x000000FF));       // BLUE
	value[3] = (char)((color & 0x0000FF00) >> 8);  // GREEN
	value[4] = (char)((color & 0x00FF0000) >> 16); // RED

}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_rgbw_value_changed(int channel_number, int color, char color_brightness, char brightness) {

	if ( devconn->srpc != NULL
		 && devconn->registered == 1 ) {

		char value[SUPLA_CHANNELVALUE_SIZE];

		supla_esp_channel_rgbw_to_value(value,color, color_brightness, brightness);
		srpc_ds_async_channel_value_changed(devconn->srpc, channel_number, value);
	}

}

#endif

char DEVCONN_ICACHE_FLASH
_supla_esp_channel_set_value(int port, char v, int channel_number) {

	char _v = v == 1 ? HI_VALUE : LO_VALUE;

	supla_esp_gpio_relay_hi(port, _v, 1);

	_v = supla_esp_gpio_relay_is_hi(port);

	supla_esp_channel_value_changed(channel_number, _v == HI_VALUE ? 1 : 0);

	return (v == 1 ? HI_VALUE : LO_VALUE) == _v;
}

void supla_esp_relay_timer_func(void *timer_arg) {

	_supla_esp_channel_set_value(((supla_relay_cfg_t*)timer_arg)->gpio_id, 0, ((supla_relay_cfg_t*)timer_arg)->channel);

}


#if defined(RGB_CONTROLLER_CHANNEL) \
    || defined(RGBW_CONTROLLER_CHANNEL) \
    || defined(RGBWW_CONTROLLER_CHANNEL) \
    || defined(DIMMER_CHANNEL)

void DEVCONN_ICACHE_FLASH supla_esp_devconn_smooth_brightness(char *brightness, char *dest_brightness) {

	if ( *brightness > *dest_brightness ) {

		 *brightness=*brightness - 2;

		 if ( *brightness < 0 || *brightness > 100 ) /* *brightness > 100 - over value fix*/
			 *brightness = 0;

		 if ( *brightness < *dest_brightness )
			 *brightness = *dest_brightness;

	 } else if ( *brightness < *dest_brightness ) {

		 *brightness=*brightness + 2;

		 if ( *brightness > 100 )
			 *brightness = 100;

		 if ( *brightness > *dest_brightness )
			 *brightness = *dest_brightness;
	 }

}

hsv DEVCONN_ICACHE_FLASH rgb2hsv(int rgb)
{
    hsv         out;
    double      min, max, delta;

    unsigned char r = (unsigned char)((rgb & 0x00FF0000) >> 16);
    unsigned char g = (unsigned char)((rgb & 0x0000FF00) >> 8);
    unsigned char b = (unsigned char)(rgb & 0x000000FF);

    min = r < g ? r : g;
    min = min  < b ? min  : b;

    max = r > g ? r : g;
    max = max  > b ? max  : b;

    out.v = max;
    delta = max - min;
    if (delta < 0.00001)
    {
        out.s = 0;
        out.h = 0;
        return out;
    }
    if( max > 0.0 ) {
        out.s = (delta / max);
    } else {
        out.s = 0.0;
        out.h = -1;
        return out;
    }
    if( r >= max )
        out.h = ( g - b ) / delta;
    else
    if( g >= max )
        out.h = 2.0 + ( b - r ) / delta;
    else
        out.h = 4.0 + ( r - g ) / delta;

    out.h *= 60.0;

    if( out.h < 0.0 )
        out.h += 360.0;

    return out;
}

int DEVCONN_ICACHE_FLASH hsv2rgb(hsv in)
{
    double      hh, p, q, t, ff;
    long        i;

    unsigned char r,g,b;
    int rgb = 0;

    if(in.s <= 0.0) {       // < is bogus, just shuts up warnings
        r = in.v;
        g = in.v;
        b = in.v;

        rgb = r & 0xFF; rgb<<=8;
        rgb |= g & 0xFF; rgb<<=8;
        rgb |= b & 0xFF;

        return rgb;
    }
    hh = in.h;
    if(hh >= 360.0) hh = 0.0;
    hh /= 60.0;
    i = (long)hh;
    ff = hh - i;
    p = in.v * (1.0 - in.s);
    q = in.v * (1.0 - (in.s * ff));
    t = in.v * (1.0 - (in.s * (1.0 - ff)));

    switch(i) {
    case 0:
        r = in.v; g = t; b = p;
        break;
    case 1:
        r = q; g = in.v; b = p;
        break;
    case 2:
        r = p; g = in.v; b = t;
        break;
    case 3:
        r = p; g = q; b = in.v;
        break;
    case 4:
        r = t; g = p; b = in.v;
        break;
    case 5:
    default:
        r = in.v; g = p; b = q;
        break;
    }

    rgb = r & 0xFF; rgb<<=8;
    rgb |= g & 0xFF; rgb<<=8;
    rgb |= b & 0xFF;

    return rgb;
}

void DEVCONN_ICACHE_FLASH supla_esp_devconn_smooth_cb(void *timer_arg) {

	 devconn_smooth *smooth = (devconn_smooth *)timer_arg;

	 if ( smooth->smoothly == 0 || smooth->counter >= 100 ) {
		 smooth->color = smooth->dest_color;
		 smooth->color_brightness = smooth->dest_color_brightness;
		 smooth->brightness = smooth->dest_brightness;
	 }

	 supla_esp_devconn_smooth_brightness(&smooth->color_brightness, &smooth->dest_color_brightness);
	 supla_esp_devconn_smooth_brightness(&smooth->brightness, &smooth->dest_brightness);

	 if ( smooth->color != smooth->dest_color ) {

		 hsv c = rgb2hsv(smooth->color);
		 hsv dc = rgb2hsv(smooth->dest_color);

		 c.s = dc.s;
		 c.v = dc.v;

		 if ( c.h < dc.h ) {

			 if ( 360-dc.h+c.h < dc.h-c.h ) {

				 c.h -= 3.6;

				 if ( c.h < 0 )
					 c.h = 359;

			 } else {

				 c.h += 3.6;

				 if ( c.h >= dc.h )
					 c.h = dc.h;
			 }

		 } else if ( c.h > dc.h ) {

			 if ( 360-c.h+dc.h < c.h-dc.h ) {

				 c.h += 3.6;

				 if ( c.h > 359 )
					 c.h = 0;

			 } else {

				 c.h -= 3.6;

				 if ( c.h < dc.h )
					 c.h = dc.h;

			 }

		 }

		 if ( c.h == dc.h ) {
			 smooth->color = smooth->dest_color;
		 } else {
			 smooth->color = hsv2rgb(c);
		 }

	 }

	 supla_esp_board_set_rgbw_value(smooth->ChannelNumber, &smooth->color, &smooth->color_brightness, &smooth->brightness);

	 if ( smooth->dest_color == smooth->color
		  && smooth->color_brightness == smooth->dest_color_brightness
		  && smooth->brightness == smooth->dest_brightness ) {

		 os_timer_disarm(&smooth->timer);

		 if ( smooth->send_value_changed ) {
			 supla_esp_channel_rgbw_value_changed(smooth->ChannelNumber, smooth->color, smooth->color_brightness, smooth->brightness);
		 }
	 }

	 smooth->counter++;

}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_set_rgbw_value(int ChannelNumber, int Color, char ColorBrightness, char Brightness, char smoothly, char send_value_changed) {

	if ( ChannelNumber >= 2 )
		return;

	 devconn_smooth *s = &smooth[ChannelNumber];

	 os_timer_disarm(&s->timer);

	 s->counter = 0;
	 s->ChannelNumber = ChannelNumber;
	 s->smoothly = smoothly;
	 s->send_value_changed = send_value_changed;

	 s->dest_color = Color;
	 s->dest_color_brightness = ColorBrightness;
	 s->dest_brightness = Brightness;

	 os_timer_setfn(&s->timer, (os_timer_func_t *)supla_esp_devconn_smooth_cb, s);
	 os_timer_arm(&s->timer, 10, 1);

}

#endif

void DEVCONN_ICACHE_FLASH
supla_esp_channel_set_value(TSD_SuplaChannelNewValue *new_value) {

#if defined(RGBW_CONTROLLER_CHANNEL) \
	|| defined(RGBWW_CONTROLLER_CHANNEL) \
	|| defined(RGB_CONTROLLER_CHANNEL) \
	|| defined(DIMMER_CHANNEL)

	unsigned char rgb_cn = 255;
	unsigned char dimmer_cn = 255;

    #ifdef RGBW_CONTROLLER_CHANNEL
	rgb_cn = RGBW_CONTROLLER_CHANNEL;
    #endif

	#ifdef RGBWW_CONTROLLER_CHANNEL
	rgb_cn = RGBWW_CONTROLLER_CHANNEL;
	dimmer_cn = RGBWW_CONTROLLER_CHANNEL+1;
	#endif

	#ifdef RGB_CONTROLLER_CHANNEL
	rgb_cn = RGB_CONTROLLER_CHANNEL;
	#endif

	#ifdef DIMMER_CHANNEL
	dimmer_cn = DIMMER_CHANNEL;
	#endif

	if ( new_value->ChannelNumber == rgb_cn
			|| new_value->ChannelNumber == dimmer_cn ) {

		int Color = 0;
		char ColorBrightness = 0;
		char Brightness = 0;

		Brightness = new_value->value[0];
		ColorBrightness = new_value->value[1];

		Color = ((int)new_value->value[4] << 16) & 0x00FF0000; // BLUE
		Color |= ((int)new_value->value[3] << 8) & 0x0000FF00; // GREEN
		Color |= (int)new_value->value[2] & 0x00000FF;         // RED

		if ( Brightness > 100 )
			Brightness = 0;

		if ( ColorBrightness > 100 )
			ColorBrightness = 0;

		if ( new_value->ChannelNumber == rgb_cn ) {

			supla_esp_state.color[0] = Color;
			supla_esp_state.color_brightness[0] = ColorBrightness;
			supla_esp_state.brightness[0] = Brightness;

		} else if ( new_value->ChannelNumber == dimmer_cn ) {

			supla_esp_state.brightness[1] = Brightness;

		}

		supla_esp_save_state(0);

		supla_esp_channel_set_rgbw_value(new_value->ChannelNumber, Color, ColorBrightness, Brightness, 1, 1);

		return;
	}

#endif


	char v = new_value->value[0];
	int a;
	char Success = 0;

/*
    char buff[200];
    ets_snprintf(buff, 200, "set_value %i,%i,%i", new_value->value[0], new_value->ChannelNumber, new_value->SenderID);
	supla_esp_write_log(buff);
*/

	for(a=0;a<RELAY_MAX_COUNT;a++)
		if ( supla_relay_cfg[a].gpio_id != 255
			 && new_value->ChannelNumber == supla_relay_cfg[a].channel ) {

			if ( supla_relay_cfg[a].bind != 255 ) {

				char s1, s2, v1, v2;

				v1 = 0;
				v2 = 0;

				if ( v == 1 ) {
					v1 = 1;
					v2 = 0;
				} else if ( v == 2 ) {
					v1 = 0;
					v2 = 1;
				}

				s1 = _supla_esp_channel_set_value(supla_relay_cfg[a].gpio_id, v1, new_value->ChannelNumber);
				s2 = _supla_esp_channel_set_value(supla_relay_cfg[supla_relay_cfg[a].bind].gpio_id, v2, new_value->ChannelNumber);

				Success = s1 != 0 || s2 != 0;

			} else {
				Success = _supla_esp_channel_set_value(supla_relay_cfg[a].gpio_id, v, new_value->ChannelNumber);
			}


			break;
		}

	srpc_ds_async_set_channel_result(devconn->srpc, new_value->ChannelNumber, new_value->SenderID, Success);

	if ( v == 1 && new_value->DurationMS > 0 ) {

		for(a=0;a<RELAY_MAX_COUNT;a++)
			if ( supla_relay_cfg[a].gpio_id != 255
				 && new_value->ChannelNumber == supla_relay_cfg[a].channel ) {

				os_timer_disarm(&supla_relay_cfg[a].timer);

				os_timer_setfn(&supla_relay_cfg[a].timer, supla_esp_relay_timer_func, &supla_relay_cfg[a]);
				os_timer_arm (&supla_relay_cfg[a].timer, new_value->DurationMS, false);

				break;
			}

	}
}

void DEVCONN_ICACHE_FLASH
supla_esp_on_remote_call_received(void *_srpc, unsigned int rr_id, unsigned int call_type, void *_dcd, unsigned char proto_version) {

	TsrpcReceivedData rd;
	char result;

	devconn->last_response = system_get_time();

	//supla_log(LOG_DEBUG, "call_received");

	if ( SUPLA_RESULT_TRUE == ( result = srpc_getdata(_srpc, &rd, 0)) ) {

		switch(rd.call_type) {
		case SUPLA_SDC_CALL_VERSIONERROR:
			supla_esp_on_version_error(rd.data.sdc_version_error);
			break;
		case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT:
			supla_esp_on_register_result(rd.data.sd_register_device_result);
			break;
		case SUPLA_SD_CALL_CHANNEL_SET_VALUE:
			supla_esp_channel_set_value(rd.data.sd_channel_new_value);
			break;
		case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:
			supla_esp_channel_set_activity_timeout_result(rd.data.sdc_set_activity_timeout_result);
			break;
		#ifdef __FOTA
		case SUPLA_SD_CALL_GET_FIRMWARE_UPDATE_URL_RESULT:
			supla_esp_update_url_result(rd.data.sc_firmware_update_url_result);
			break;
		#endif
		}

		srpc_rd_free(&rd);

	} else if ( result == SUPLA_RESULT_DATA_ERROR ) {

		supla_log(LOG_DEBUG, "DATA ERROR!");
	}

}

void
supla_esp_devconn_iterate(void *timer_arg) {

	if ( devconn->srpc != NULL ) {

		if ( devconn->registered == 0 ) {
			devconn->registered = -1;

			TDS_SuplaRegisterDevice_B srd;
			memset(&srd, 0, sizeof(TDS_SuplaRegisterDevice_B));

			srd.channel_count = 0;
			srd.LocationID = supla_esp_cfg.LocationID;
			ets_snprintf(srd.LocationPWD, SUPLA_LOCATION_PWD_MAXSIZE, "%s", supla_esp_cfg.LocationPwd);
			ets_snprintf(srd.ServerName, SUPLA_SERVER_NAME_MAXSIZE, "%s", supla_esp_cfg.Server);

			supla_esp_board_set_device_name(srd.Name, SUPLA_DEVICE_NAME_MAXSIZE);

			strcpy(srd.SoftVer, SUPLA_ESP_SOFTVER);
			os_memcpy(srd.GUID, supla_esp_cfg.GUID, SUPLA_GUID_SIZE);

			//supla_log(LOG_DEBUG, "LocationID=%i, LocationPWD=%s", srd.LocationID, srd.LocationPWD);

			supla_esp_board_set_channels(&srd);

			srpc_ds_async_registerdevice_b(devconn->srpc, &srd);

		};

		supla_esp_data_write(NULL, 0, NULL);

		if( srpc_iterate(devconn->srpc) == SUPLA_RESULT_FALSE ) {
			supla_log(LOG_DEBUG, "iterate fail");
			supla_esp_devconn_system_restart();
		}

	}

}


void DEVCONN_ICACHE_FLASH
supla_esp_srpc_free(void) {

	os_timer_disarm(&devconn->supla_iterate_timer);

	devconn->registered = 0;
	devconn->last_response = 0;

	if ( devconn->srpc != NULL ) {
		srpc_free(devconn->srpc);
		devconn->srpc = NULL;
	}
}

void DEVCONN_ICACHE_FLASH
supla_esp_srpc_init(void) {
	
	supla_esp_srpc_free();
		
	TsrpcParams srpc_params;
	srpc_params_init(&srpc_params);
	srpc_params.data_read = &supla_esp_data_read;
	srpc_params.data_write = &supla_esp_data_write;
	srpc_params.on_remote_call_received = &supla_esp_on_remote_call_received;

	devconn->srpc = srpc_init(&srpc_params);
	
	os_timer_setfn(&devconn->supla_iterate_timer, (os_timer_func_t *)supla_esp_devconn_iterate, NULL);
	os_timer_arm(&devconn->supla_iterate_timer, 100, 1);

}

void DEVCONN_ICACHE_FLASH
supla_espconn_disconnect(struct espconn *espconn) {
	
	//supla_log(LOG_DEBUG, "Disconnect %i", espconn->state);
	
	if ( espconn->state != ESPCONN_CLOSE
		 && espconn->state != ESPCONN_NONE ) {
		_supla_espconn_disconnect(espconn);
	}
	
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_connect_cb(void *arg) {
	//supla_log(LOG_DEBUG, "devconn_connect_cb\r\n");
	supla_esp_srpc_init();	
	devconn->autoconnect = 1;
}


void DEVCONN_ICACHE_FLASH
supla_esp_devconn_disconnect_cb(void *arg){
	//supla_log(LOG_DEBUG, "devconn_disconnect_cb\r\n");

	devconn->autoconnect = 1;

	 if ( supla_esp_cfgmode_started() == 0
		  && supla_esp_devconn_update_started() == 0 ) {

			supla_esp_srpc_free();
			supla_esp_wifi_check_status(devconn->autoconnect);

	 }

}


void DEVCONN_ICACHE_FLASH
supla_esp_devconn_dns_found_cb(const char *name, ip_addr_t *ip, void *arg) {

	if ( ip == NULL ) {
		supla_esp_set_state(LOG_NOTICE, "Domain not found.");
		return;

	}

	supla_espconn_disconnect(&devconn->ESPConn);

	devconn->ESPConn.proto.tcp = &devconn->ESPTCP;
	devconn->ESPConn.type = ESPCONN_TCP;
	devconn->ESPConn.state = ESPCONN_NONE;

	os_memcpy(devconn->ESPConn.proto.tcp->remote_ip, ip, 4);
	devconn->ESPConn.proto.tcp->local_port = espconn_port();

	#if NOSSL == 1
		devconn->ESPConn.proto.tcp->remote_port = 2015;
	#else
		devconn->ESPConn.proto.tcp->remote_port = 2016;
	#endif

	espconn_regist_recvcb(&devconn->ESPConn, supla_esp_devconn_recv_cb);
	espconn_regist_connectcb(&devconn->ESPConn, supla_esp_devconn_connect_cb);
	espconn_regist_disconcb(&devconn->ESPConn, supla_esp_devconn_disconnect_cb);

	supla_espconn_connect(&devconn->ESPConn);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_resolvandconnect(void) {

	devconn->autoconnect = 0;

	supla_espconn_disconnect(&devconn->ESPConn);

	uint32_t _ip = ipaddr_addr(supla_esp_cfg.Server);

	if ( _ip == -1 ) {
		 supla_log(LOG_DEBUG, "Resolv %s", supla_esp_cfg.Server);

		 espconn_gethostbyname(&devconn->ESPConn, supla_esp_cfg.Server, &devconn->ipaddr, supla_esp_devconn_dns_found_cb);
	} else {
		 supla_esp_devconn_dns_found_cb(supla_esp_cfg.Server, (ip_addr_t *)&_ip, NULL);
	}


}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_watchdog_cb(void *timer_arg) {

	 if ( supla_esp_cfgmode_started() == 0
		  && supla_esp_devconn_update_started() == 0 ) {

			unsigned int t = system_get_time();

			if ( t > devconn->last_response && t-devconn->last_response > 60000000 ) {
				supla_log(LOG_DEBUG, "WATCHDOG TIMEOUT");
				supla_esp_devconn_system_restart();
			}

	 }

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_before_cfgmode_start(void) {

	os_timer_disarm(&devconn->supla_watchdog_timer);
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_before_update_start(void) {

	os_timer_disarm(&devconn->supla_watchdog_timer);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_init(void) {

	if ( devconn == NULL ) {
		devconn = (devconn_params*)malloc(sizeof(devconn_params));
		memset(devconn, 0, sizeof(devconn_params));
	}


	memset(&devconn->ESPConn, 0, sizeof(struct espconn));
	memset(&devconn->ESPTCP, 0, sizeof(esp_tcp));
	
	devconn->last_response = 0;
	devconn->autoconnect = 1;
	devconn->last_wifi_status = STATION_GOT_IP+1;

	ets_snprintf(devconn->laststate, STATE_MAXSIZE, "WiFi - Connecting...");
	//sys_wait_for_restart = 0;

	os_timer_disarm(&devconn->supla_watchdog_timer);
	os_timer_setfn(&devconn->supla_watchdog_timer, (os_timer_func_t *)supla_esp_devconn_watchdog_cb, NULL);
	os_timer_arm(&devconn->supla_watchdog_timer, 1000, 1);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_start(void) {
	
	wifi_station_disconnect();
	
	supla_esp_gpio_state_disconnected();

    struct station_config stationConf;

    wifi_set_opmode( STATION_MODE );

	#ifdef WIFI_SLEEP_DISABLE
		wifi_set_sleep_type(NONE_SLEEP_T);
	#endif

    os_memcpy(stationConf.ssid, supla_esp_cfg.WIFI_SSID, WIFI_SSID_MAXSIZE);
    os_memcpy(stationConf.password, supla_esp_cfg.WIFI_PWD, WIFI_PWD_MAXSIZE);
   
    stationConf.ssid[31] = 0;
    stationConf.password[63] = 0;
    
    
    wifi_station_set_config(&stationConf);
    wifi_station_set_auto_connect(1);

    wifi_station_connect();
    
	os_timer_disarm(&devconn->supla_devconn_timer1);
	os_timer_setfn(&devconn->supla_devconn_timer1, (os_timer_func_t *)supla_esp_devconn_timer1_cb, NULL);
	os_timer_arm(&devconn->supla_devconn_timer1, 1000, 1);
	
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_stop(void) {
	
	os_timer_disarm(&devconn->supla_devconn_timer1);
	supla_espconn_disconnect(&devconn->ESPConn);
	supla_esp_wifi_check_status(0);
}

char * DEVCONN_ICACHE_FLASH
supla_esp_devconn_laststate(void) {
	return devconn->laststate;
}

void DEVCONN_ICACHE_FLASH
supla_esp_wifi_check_status(char autoconnect) {

	uint8 status = wifi_station_get_connect_status();

	if ( status != devconn->last_wifi_status )
		supla_log(LOG_DEBUG, "WiFi Status: %i", status);

	devconn->last_wifi_status = status;

	if ( STATION_GOT_IP == status ) {

		if ( devconn->srpc == NULL ) {

			supla_esp_gpio_state_ipreceived();

			 if ( autoconnect == 1 )
				 supla_esp_devconn_resolvandconnect();
		}


	} else {

		switch(status) {

			case STATION_NO_AP_FOUND:
				supla_esp_set_state(LOG_NOTICE, "SSID Not found");
				break;
			case STATION_WRONG_PASSWORD:
				supla_esp_set_state(LOG_NOTICE, "WiFi - Wrong password");
				break;
		}

		supla_esp_gpio_state_disconnected();

	}

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_timer1_cb(void *timer_arg) {

	supla_esp_wifi_check_status(devconn->autoconnect);

	unsigned int t1;
	unsigned int t2;

	//supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

	if ( devconn->registered == 1
		 && devconn->server_activity_timeout > 0
		 && devconn->srpc != NULL ) {

		    t1 = system_get_time();
		    t2 = abs((t1-devconn->last_response)/1000000);

		    if ( t2 >= (devconn->server_activity_timeout+10) ) {

		    	supla_log(LOG_DEBUG, "Response timeout %i, %i, %i, %i",  t1, devconn->last_response, (t1-devconn->last_response)/1000000, devconn->server_activity_timeout+5);

		    	supla_esp_srpc_free();
		    	supla_esp_wifi_check_status(devconn->autoconnect);

		    } else if ( t2 >= (devconn->server_activity_timeout-5)
		    		    && t2 <= devconn->server_activity_timeout ) {

		    	//supla_log(LOG_DEBUG, "PING");
		    	//system_print_meminfo();

				srpc_dcs_async_ping_server(devconn->srpc);
				
			}

	}
}

#if defined(TEMPERATURE_CHANNEL) || defined(TEMPERATURE_HUMIDITY_CHANNEL)

void DEVCONN_ICACHE_FLASH supla_esp_devconn_on_temp_humidity_changed(char humidity) {

	if ( devconn->srpc != NULL
		 && devconn->registered == 1 ) {

		char value[SUPLA_CHANNELVALUE_SIZE];

        #if defined(TEMPERATURE_CHANNEL)

		    memset(value, 0, sizeof(SUPLA_CHANNELVALUE_SIZE));

			supla_get_temperature(value);
			srpc_ds_async_channel_value_changed(devconn->srpc, TEMPERATURE_CHANNEL, value);

		#endif

        #if defined(TEMPERATURE_HUMIDITY_CHANNEL)

			memset(value, 0, sizeof(SUPLA_CHANNELVALUE_SIZE));

			supla_get_temp_and_humidity(value);
			srpc_ds_async_channel_value_changed(devconn->srpc, TEMPERATURE_HUMIDITY_CHANNEL, value);

		#endif


	}

}

#endif


