/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "SDL_hints.h"
#include "SDL_events.h"
#include "SDL_joystick.h"
#include "SDL_gamecontroller.h"
#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"


#ifdef SDL_JOYSTICK_HIDAPI_PS5

/* Define this if you want to log all packets from the controller */
/*#define DEBUG_PS5_PROTOCOL*/

typedef enum
{
    k_EPS5ReportIdState = 0x01,
    k_EPS5ReportIdBluetoothState = 0x31,
} EPS5ReportId;

typedef enum
{
    k_EPS5FeatureReportIdSerialNumber = 0x09,
} EPS5FeatureReportId;

typedef struct
{
    Uint8 ucLeftJoystickX;
    Uint8 ucLeftJoystickY;
    Uint8 ucRightJoystickX;
    Uint8 ucRightJoystickY;
    Uint8 rgucButtonsHatAndCounter[3];
    Uint8 ucTriggerLeft;
    Uint8 ucTriggerRight;
} PS5SimpleStatePacket_t;

typedef struct
{
    Uint8 ucLeftJoystickX;              /* 0 */
    Uint8 ucLeftJoystickY;              /* 1 */
    Uint8 ucRightJoystickX;             /* 2 */
    Uint8 ucRightJoystickY;             /* 3 */
    Uint8 ucTriggerLeft;                /* 4 */
    Uint8 ucTriggerRight;               /* 5 */
    Uint8 ucCounter;                    /* 6 */
    Uint8 rgucButtonsAndHat[3];         /* 7 */
    Uint8 ucZero;                       /* 10 */
    Uint8 rgucPacketSequence[4];        /* 11 - 32 bit little endian */
    Uint8 rgucAccel[6];                 /* 15 */
    Uint8 rgucGyro[6];                  /* 21 */
    Uint8 rgucTimer1[4];                /* 27 - 32 bit little endian */
    Uint8 ucBatteryTemp;                /* 31 */
    Uint8 ucTouchpadCounter1;           /* 32 - high bit clear + counter */
    Uint8 rgucTouchpadData1[3];         /* 33 - X/Y, 12 bits per axis */
    Uint8 ucTouchpadCounter2;           /* 36 - high bit clear + counter */
    Uint8 rgucTouchpadData2[3];         /* 37 - X/Y, 12 bits per axis */
    Uint8 rgucUnknown1[8];              /* 40 */
    Uint8 rgucTimer2[4];                /* 48 - 32 bit little endian */
    Uint8 ucBatteryLevel;               /* 52 */
    Uint8 ucConnectState;               /* 53 - 0x08 = USB, 0x03 = headphone */

    /* There's more unknown data at the end, and a 32-bit CRC on Bluetooth */
} PS5StatePacket_t;


static void ReadFeatureReport(hid_device *dev, Uint8 report_id)
{
    Uint8 report[USB_PACKET_LENGTH + 1];
    int size;

    SDL_memset(report, 0, sizeof(report));
    report[0] = report_id;
    size = hid_get_feature_report(dev, report, sizeof(report));
    if (size > 0) {
#ifdef DEBUG_PS5_PROTOCOL
        SDL_Log("Report %d\n", report_id);
        HIDAPI_DumpPacket("Report: size = %d", report, size);
#endif
    }
}

typedef struct {
    union
    {
        PS5SimpleStatePacket_t simple;
        PS5StatePacket_t state;
    } last_state;
} SDL_DriverPS5_Context;


static SDL_bool
HIDAPI_DriverPS5_IsSupportedDevice(const char *name, SDL_GameControllerType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return (type == SDL_CONTROLLER_TYPE_PS5);
}

static const char *
HIDAPI_DriverPS5_GetDeviceName(Uint16 vendor_id, Uint16 product_id)
{
    if (vendor_id == USB_VENDOR_SONY) {
        return "PS5 Controller";
    }
    return NULL;
}

static SDL_bool
HIDAPI_DriverPS5_InitDevice(SDL_HIDAPI_Device *device)
{
    return HIDAPI_JoystickConnected(device, NULL, SDL_FALSE);
}

static int
HIDAPI_DriverPS5_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void
HIDAPI_DriverPS5_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
}

static SDL_bool
HIDAPI_DriverPS5_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverPS5_Context *ctx;

    ctx = (SDL_DriverPS5_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        SDL_OutOfMemory();
        return SDL_FALSE;
    }

    device->dev = hid_open_path(device->path, 0);
    if (!device->dev) {
        SDL_free(ctx);
        SDL_SetError("Couldn't open %s", device->path);
        return SDL_FALSE;
    }
    device->context = ctx;

    /* Read the serial number (Bluetooth address in reverse byte order)
       This will also enable enhanced reports over Bluetooth
    */
    ReadFeatureReport(device->dev, k_EPS5FeatureReportIdSerialNumber);

    /* Initialize the joystick capabilities */
    joystick->nbuttons = 17;
    joystick->naxes = SDL_CONTROLLER_AXIS_MAX;
    joystick->epowerlevel = SDL_JOYSTICK_POWER_WIRED;

    SDL_PrivateJoystickAddTouchpad(joystick, 2);

    return SDL_TRUE;
}

static int
HIDAPI_DriverPS5_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    return SDL_Unsupported();
}

static int
HIDAPI_DriverPS5_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static SDL_bool
HIDAPI_DriverPS5_HasJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    return SDL_FALSE;
}

static int
HIDAPI_DriverPS5_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static void
HIDAPI_DriverPS5_HandleSimpleStatePacket(SDL_Joystick *joystick, hid_device *dev, SDL_DriverPS5_Context *ctx, PS5SimpleStatePacket_t *packet)
{
    Sint16 axis;

    if (ctx->last_state.simple.rgucButtonsHatAndCounter[0] != packet->rgucButtonsHatAndCounter[0]) {
        {
            Uint8 data = (packet->rgucButtonsHatAndCounter[0] >> 4);

            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_X, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_A, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_B, (data & 0x04) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_Y, (data & 0x08) ? SDL_PRESSED : SDL_RELEASED);
        }
        {
            Uint8 data = (packet->rgucButtonsHatAndCounter[0] & 0x0F);
            SDL_bool dpad_up = SDL_FALSE;
            SDL_bool dpad_down = SDL_FALSE;
            SDL_bool dpad_left = SDL_FALSE;
            SDL_bool dpad_right = SDL_FALSE;

            switch (data) {
            case 0:
                dpad_up = SDL_TRUE;
                break;
            case 1:
                dpad_up = SDL_TRUE;
                dpad_right = SDL_TRUE;
                break;
            case 2:
                dpad_right = SDL_TRUE;
                break;
            case 3:
                dpad_right = SDL_TRUE;
                dpad_down = SDL_TRUE;
                break;
            case 4:
                dpad_down = SDL_TRUE;
                break;
            case 5:
                dpad_left = SDL_TRUE;
                dpad_down = SDL_TRUE;
                break;
            case 6:
                dpad_left = SDL_TRUE;
                break;
            case 7:
                dpad_up = SDL_TRUE;
                dpad_left = SDL_TRUE;
                break;
            default:
                break;
            }
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, dpad_down);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_UP, dpad_up);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, dpad_right);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, dpad_left);
        }
    }

    if (ctx->last_state.simple.rgucButtonsHatAndCounter[1] != packet->rgucButtonsHatAndCounter[1]) {
        Uint8 data = packet->rgucButtonsHatAndCounter[1];

        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_BACK, (data & 0x10) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_START, (data & 0x20) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSTICK, (data & 0x40) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSTICK, (data & 0x80) ? SDL_PRESSED : SDL_RELEASED);
    }

    if (ctx->last_state.simple.rgucButtonsHatAndCounter[2] != packet->rgucButtonsHatAndCounter[2]) {
        Uint8 data = (packet->rgucButtonsHatAndCounter[2] & 0x03);

        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_GUIDE, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, 15, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
    }

    axis = ((int)packet->ucTriggerLeft * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, axis);
    axis = ((int)packet->ucTriggerRight * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, axis);
    axis = ((int)packet->ucLeftJoystickX * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, axis);
    axis = ((int)packet->ucLeftJoystickY * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, axis);
    axis = ((int)packet->ucRightJoystickX * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, axis);
    axis = ((int)packet->ucRightJoystickY * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, axis);

    SDL_memcpy(&ctx->last_state.simple, packet, sizeof(ctx->last_state.simple));
}

static void
HIDAPI_DriverPS5_HandleStatePacket(SDL_Joystick *joystick, hid_device *dev, SDL_DriverPS5_Context *ctx, PS5StatePacket_t *packet)
{
    static const float TOUCHPAD_SCALEX = 1.0f / 1920;
    static const float TOUCHPAD_SCALEY = 1.0f / 1070;
    Sint16 axis;
    Uint8 touchpad_state;
    int touchpad_x, touchpad_y;

    if (ctx->last_state.state.rgucButtonsAndHat[0] != packet->rgucButtonsAndHat[0]) {
        {
            Uint8 data = (packet->rgucButtonsAndHat[0] >> 4);

            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_X, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_A, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_B, (data & 0x04) ? SDL_PRESSED : SDL_RELEASED);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_Y, (data & 0x08) ? SDL_PRESSED : SDL_RELEASED);
        }
        {
            Uint8 data = (packet->rgucButtonsAndHat[0] & 0x0F);
            SDL_bool dpad_up = SDL_FALSE;
            SDL_bool dpad_down = SDL_FALSE;
            SDL_bool dpad_left = SDL_FALSE;
            SDL_bool dpad_right = SDL_FALSE;

            switch (data) {
            case 0:
                dpad_up = SDL_TRUE;
                break;
            case 1:
                dpad_up = SDL_TRUE;
                dpad_right = SDL_TRUE;
                break;
            case 2:
                dpad_right = SDL_TRUE;
                break;
            case 3:
                dpad_right = SDL_TRUE;
                dpad_down = SDL_TRUE;
                break;
            case 4:
                dpad_down = SDL_TRUE;
                break;
            case 5:
                dpad_left = SDL_TRUE;
                dpad_down = SDL_TRUE;
                break;
            case 6:
                dpad_left = SDL_TRUE;
                break;
            case 7:
                dpad_up = SDL_TRUE;
                dpad_left = SDL_TRUE;
                break;
            default:
                break;
            }
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN, dpad_down);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_UP, dpad_up);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, dpad_right);
            SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT, dpad_left);
        }
    }

    if (ctx->last_state.state.rgucButtonsAndHat[1] != packet->rgucButtonsAndHat[1]) {
        Uint8 data = packet->rgucButtonsAndHat[1];

        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_BACK, (data & 0x10) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_START, (data & 0x20) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSTICK, (data & 0x40) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSTICK, (data & 0x80) ? SDL_PRESSED : SDL_RELEASED);
    }

    if (ctx->last_state.state.rgucButtonsAndHat[2] != packet->rgucButtonsAndHat[2]) {
        Uint8 data = packet->rgucButtonsAndHat[2];

        SDL_PrivateJoystickButton(joystick, SDL_CONTROLLER_BUTTON_GUIDE, (data & 0x01) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, 15, (data & 0x04) ? SDL_PRESSED : SDL_RELEASED);
        SDL_PrivateJoystickButton(joystick, 16, (data & 0x02) ? SDL_PRESSED : SDL_RELEASED);
    }

    axis = ((int)packet->ucTriggerLeft * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, axis);
    axis = ((int)packet->ucTriggerRight * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, axis);
    axis = ((int)packet->ucLeftJoystickX * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, axis);
    axis = ((int)packet->ucLeftJoystickY * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, axis);
    axis = ((int)packet->ucRightJoystickX * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, axis);
    axis = ((int)packet->ucRightJoystickY * 257) - 32768;
    SDL_PrivateJoystickAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, axis);

    if (packet->ucBatteryLevel & 0x10) {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_WIRED;
    } else {
        /* Battery level ranges from 0 to 10 */
        int level = (packet->ucBatteryLevel & 0xF);
        if (level == 0) {
            joystick->epowerlevel = SDL_JOYSTICK_POWER_EMPTY;
        } else if (level <= 2) {
            joystick->epowerlevel = SDL_JOYSTICK_POWER_LOW;
        } else if (level <= 7) {
            joystick->epowerlevel = SDL_JOYSTICK_POWER_MEDIUM;
        } else {
            joystick->epowerlevel = SDL_JOYSTICK_POWER_FULL;
        }
    }

    touchpad_state = ((packet->ucTouchpadCounter1 & 0x80) == 0) ? SDL_PRESSED : SDL_RELEASED;
    touchpad_x = packet->rgucTouchpadData1[0] | (((int)packet->rgucTouchpadData1[1] & 0x0F) << 8);
    touchpad_y = (packet->rgucTouchpadData1[1] >> 4) | ((int)packet->rgucTouchpadData1[2] << 4);
    SDL_PrivateJoystickTouchpad(joystick, 0, 0, touchpad_state, touchpad_x * TOUCHPAD_SCALEX, touchpad_y * TOUCHPAD_SCALEY, touchpad_state ? 1.0f : 0.0f);

    touchpad_state = ((packet->ucTouchpadCounter2 & 0x80) == 0) ? SDL_PRESSED : SDL_RELEASED;
    touchpad_x = packet->rgucTouchpadData2[0] | (((int)packet->rgucTouchpadData2[1] & 0x0F) << 8);
    touchpad_y = (packet->rgucTouchpadData2[1] >> 4) | ((int)packet->rgucTouchpadData2[2] << 4);
    SDL_PrivateJoystickTouchpad(joystick, 0, 1, touchpad_state, touchpad_x * TOUCHPAD_SCALEX, touchpad_y * TOUCHPAD_SCALEY, touchpad_state ? 1.0f : 0.0f);

    SDL_memcpy(&ctx->last_state.state, packet, sizeof(ctx->last_state.state));
}

static SDL_bool
HIDAPI_DriverPS5_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverPS5_Context *ctx = (SDL_DriverPS5_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH*2];
    int size;

    if (device->num_joysticks > 0) {
        joystick = SDL_JoystickFromInstanceID(device->joysticks[0]);
    }
    if (!joystick) {
        return SDL_FALSE;
    }

    while ((size = hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_PS5_PROTOCOL
        HIDAPI_DumpPacket("PS5 packet: size = %d", data, size);
#endif
        switch (data[0]) {
        case k_EPS5ReportIdState:
            if (size == 10) {
                HIDAPI_DriverPS5_HandleSimpleStatePacket(joystick, device->dev, ctx, (PS5SimpleStatePacket_t *)&data[1]);
            } else {
                HIDAPI_DriverPS5_HandleStatePacket(joystick, device->dev, ctx, (PS5StatePacket_t *)&data[1]);
            }
            break;
        case k_EPS5ReportIdBluetoothState:
            HIDAPI_DriverPS5_HandleStatePacket(joystick, device->dev, ctx, (PS5StatePacket_t *)&data[2]);
            break;
        default:
#ifdef DEBUG_JOYSTICK
            SDL_Log("Unknown PS5 packet: 0x%.2x\n", data[0]);
#endif
            break;
        }
    }

    if (size < 0) {
        /* Read error, device is disconnected */
        HIDAPI_JoystickDisconnected(device, joystick->instance_id, SDL_FALSE);
    }
    return (size >= 0);
}

static void
HIDAPI_DriverPS5_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    hid_close(device->dev);
    device->dev = NULL;

    SDL_free(device->context);
    device->context = NULL;
}

static void
HIDAPI_DriverPS5_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverPS5 =
{
    SDL_HINT_JOYSTICK_HIDAPI_PS5,
    SDL_TRUE,
    HIDAPI_DriverPS5_IsSupportedDevice,
    HIDAPI_DriverPS5_GetDeviceName,
    HIDAPI_DriverPS5_InitDevice,
    HIDAPI_DriverPS5_GetDevicePlayerIndex,
    HIDAPI_DriverPS5_SetDevicePlayerIndex,
    HIDAPI_DriverPS5_UpdateDevice,
    HIDAPI_DriverPS5_OpenJoystick,
    HIDAPI_DriverPS5_RumbleJoystick,
    HIDAPI_DriverPS5_RumbleJoystickTriggers,
    HIDAPI_DriverPS5_HasJoystickLED,
    HIDAPI_DriverPS5_SetJoystickLED,
    HIDAPI_DriverPS5_CloseJoystick,
    HIDAPI_DriverPS5_FreeDevice,
    NULL
};

#endif /* SDL_JOYSTICK_HIDAPI_PS5 */

#endif /* SDL_JOYSTICK_HIDAPI */

/* vi: set ts=4 sw=4 expandtab: */
