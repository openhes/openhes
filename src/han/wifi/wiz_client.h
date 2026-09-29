////////////////////////////////////////////////////////////////////////////////
// Copyright 2026 Tom G. Huang <tomghuang@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Minimal client for the Philips WiZ smart bulbs' native local-network
/// protocol: JSON messages over UDP, port 38899.
///
/// @details
/// This is a real, documented protocol -- e.g.:
///   {"method":"setPilot","params":{"state":true}}
///   {"method":"setPilot","params":{"state":false}}
///   {"method":"setPilot","params":{"r":255,"g":0,"b":0}}
///   {"method":"getPilot","params":{}}
///
/// We send these JSON data as a UDP datagram to the bulb's IP on port 38899;
/// the bulb replies with a JSON datagram on the same socket.

#ifndef HES_WIZ_CLIENT_H
#define HES_WIZ_CLIENT_H

#include <stdbool.h>

typedef struct wiz_client {
    int fd;
    char ip[64];
    int port;
} wiz_client_t;

////////////////////////////////////////////////////////////////////////////////
/// Opens the UDP socket.
///
/// @param c Pointer to a wiz_client_t struct to initialize.
/// @param ip The IP address of the WiZ bulb.
/// @param port The port number of the WiZ bulb (typically 38899).
/// @return 0 on success, non-zero on failure.
int wiz_client_open(wiz_client_t* c, const char* ip, int port);

////////////////////////////////////////////////////////////////////////////////
/// Turns on/off the bulb.
///
/// Sends {"method":"setPilot","params":{"state":<on>}} and waits (with a short
/// timeout) for the bulb's acknowledgement.
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param on Boolean value indicating the desired state (true for on, false for off).
/// @return 0 on success, non-zero on failure.
int wiz_client_set_state(wiz_client_t* c, bool on);

////////////////////////////////////////////////////////////////////////////////
/// Gets the state of the bulb.
///
/// Sends {"method":"getPilot","params":{}} and parses "state" out of the reply
/// into *out_on. Returns 0 on success.
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param out_on Pointer to a boolean variable to store the bulb's state.
/// @return 0 on success, non-zero on failure.
int wiz_client_get_state(wiz_client_t* c, bool* out_on);

////////////////////////////////////////////////////////////////////////////////
/// Sets the RGB color of the bulb.
///
/// Sends {"method":"setPilot","params":{"r":<r>,"g":<g>,"b":<b>}} and waits
/// (with a short timeout) for the bulb's acknowledgement.
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param r Red component, 0-255.
/// @param g Green component, 0-255.
/// @param b Blue component, 0-255.
/// @return 0 on success, non-zero on failure.
int wiz_client_set_rgb(wiz_client_t* c, int r, int g, int b);

////////////////////////////////////////////////////////////////////////////////
/// Gets the RGB color of the bulb.
///
/// Sends {"method":"getPilot","params":{}} and parses the "r"/"g"/"b" fields
/// out of the reply. Returns 0 on success.
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param out_r Pointer to store the red component, 0-255.
/// @param out_g Pointer to store the green component, 0-255.
/// @param out_b Pointer to store the blue component, 0-255.
/// @return 0 on success, non-zero on failure.
int wiz_client_get_rgb(wiz_client_t* c, int* out_r, int* out_g, int* out_b);

////////////////////////////////////////////////////////////////////////////////
/// Sets the bulb's brightness level, 0-100.
///
/// Brightness is the bulb's own "dimming" parameter, sent as
/// {"method":"setPilot","params":{"dimming":<level>}}. It is quite separate
/// from the r/g/b colour channels -- setting r/g/b alone changes the colour but
/// not the actual brightness. The WiZ dimming range is 10-100, so levels below
/// 10 are clamped up to 10.
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param level Desired brightness, 0 (off) to 100 (full).
/// @return 0 on success, non-zero on failure.
int wiz_client_set_brightness(wiz_client_t* c, int level);

////////////////////////////////////////////////////////////////////////////////
/// Gets the bulb's brightness level, 0-100.
///
/// Reads the "dimming" field from the getPilot reply (falling back to the peak
/// RGB channel if the reply has no "dimming").
///
/// @param c Pointer to an initialized wiz_client_t struct.
/// @param out_level Pointer to store the brightness percentage, 0-100.
/// @return 0 on success, non-zero on failure.
int wiz_client_get_brightness(wiz_client_t* c, int* out_level);

////////////////////////////////////////////////////////////////////////////////
/// Closes the UDP socket.
///
/// @param c Pointer to an initialized wiz_client_t struct.
void wiz_client_close(wiz_client_t* c);

#endif  // HES_WIZ_CLIENT_H
