/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "base/EventTypes.h"

#include <cstdint>
#include <string>

// protocol version number
// 1.0:  initial protocol
// 1.1:  adds KeyCode to key press, release, and repeat
// 1.2:  adds mouse relative motion
// 1.3:  adds keep alive and deprecates heartbeats,
//       adds horizontal mouse scrolling
// 1.4:  adds crypto support
// 1.5:  adds file transfer and removes home brew crypto
// 1.6:  adds clipboard streaming
// 1.7:  adds transactional input handoff readiness
// 1.8:  adds input epochs, per-connection input sequence, and explicit
//       keyboard broadcast routing
// 1.9:  adds a separately authenticated bulk payload connection
// 1.10: adds positive acknowledgment after a committed input handoff
// 1.11: adds acknowledged source input-lease revocation before handoff commit
// NOTE: with new version, barrier minor version should increment
static const SInt16        kProtocolMajorVersion = 1;
static const SInt16        kProtocolMinorVersion = 12;
static const SInt16        kProtocolMinimumMinorVersion = 12;

// default contact port number
static const UInt16        kDefaultPort = 24800;

// maximum total length for greeting returned by client
static const UInt32        kMaxHelloLength = 1024;

// time between kMsgCKeepAlive (in seconds).  a non-positive value disables
// keep alives.  this is the default rate that can be overridden using an
// option.
static const double        kKeepAliveRate = 3.0;

// number of skipped kMsgCKeepAlive messages that indicates a problem.
// Desktop switches and secure-desktop transitions can briefly stall the
// event loop even while the connection remains healthy. Pending stream work
// has its own progress checks, so tolerate five missed idle keepalives before
// retiring the socket.
static const double        kKeepAlivesUntilDeath = 5.0;

// obsolete heartbeat stuff
static const double        kHeartRate = -1.0;
static const double        kHeartBeatsUntilDeath = 3.0;

// Messages of very large size indicate a likely protocol error. We don't parse such messages and
// drop connection instead. Note that e.g. the clipboard messages are already limited to 32kB.
static constexpr std::uint32_t PROTOCOL_MAX_MESSAGE_LENGTH = 4 * 1024 * 1024;
static constexpr std::uint32_t PROTOCOL_MAX_LIST_LENGTH = 1024 * 1024;
static constexpr std::uint32_t PROTOCOL_MAX_STRING_LENGTH = 1024 * 1024;

// direction constants
enum EDirection {
    kNoDirection,
    kLeft,
    kRight,
    kTop,
    kBottom,
    kFirstDirection = kLeft,
    kLastDirection = kBottom,
    kNumDirections = kLastDirection - kFirstDirection + 1
};
enum EDirectionMask {
    kNoDirMask  = 0,
    kLeftMask   = 1 << kLeft,
    kRightMask  = 1 << kRight,
    kTopMask    = 1 << kTop,
    kBottomMask = 1 << kBottom
};

// Data transfer constants
enum EDataTransfer {
    kDataStart = 1,
    kDataChunk = 2,
    kDataEnd = 3,
    kDataCancel = 4
};

// Data received constants
enum EDataReceived {
    kStart,
    kNotFinish,
    kBackpressure,
    kFinish,
    kError,
    kCancelled
};

enum EInputMessageFlags {
    kInputMessageNoFlags = 0,
    kInputMessageBroadcast = 1
};

//
// message codes (trailing NUL is not part of code).  in comments, $n
// refers to the n'th argument (counting from one).  message codes are
// always 4 bytes optionally followed by message specific parameters
// except those for the greeting handshake.
//

//
// positions and sizes are signed 16 bit integers.
//

//
// greeting handshake messages
//

// say hello to client;  primary -> secondary
// $1 = protocol major version number supported by server.  $2 =
// protocol minor version number supported by server.
extern const char*        kMsgHello;

// respond to hello from server;  secondary -> primary
// $1 = protocol major version number supported by client.  $2 =
// protocol minor version number supported by client.  $3 = client
// name.
extern const char*        kMsgHelloBack;

// identify a secondary bulk connection; secondary -> primary
// $1 = major, $2 = minor, $3 = client name, $4 = one-time binding token.
extern const char*        kMsgHelloBulkBack;

// identify a protocol 1.12 bulk connection; secondary -> primary
// $1 = major, $2 = minor, $3 = client name, $4 = one-time token,
// $5 = stable control-connection binding.
extern const char*        kMsgHelloBulkBack1_12;


//
// command codes
//

// no operation;  secondary -> primary
extern const char*        kMsgCNoop;

// close connection;  primary -> secondary
extern const char*        kMsgCClose;

// enter screen:  primary -> secondary
// entering screen at screen position $1 = x, $2 = y.  x,y are
// absolute screen coordinates.  $3 = sequence number, which is
// used to order messages between screens.  the secondary screen
// must return this number with some messages.  $4 = modifier key
// mask.  this will have bits set for each toggle modifier key
// that is activated on entry to the screen.  the secondary screen
// should adjust its toggle modifiers to reflect that state.
extern const char*        kMsgCEnter;

// prepare to enter screen: primary -> secondary
// $1 = x, $2 = y, $3 = sequence number, $4 = modifier mask.
extern const char*        kMsgCPrepareEnter;

// abort a prepared enter: primary -> secondary
// $1 = sequence number.
extern const char*        kMsgCAbortEnter;

// revoke an active source input lease: primary -> secondary
// $1 = handoff sequence, $2 = expected active input epoch.
extern const char*        kMsgCRevokeInput;

// leave screen:  primary -> secondary
// leaving screen.  the secondary screen should send clipboard
// data in response to this message for those clipboards that
// it has grabbed (i.e. has sent a kMsgCClipboard for and has
// not received a kMsgCClipboard for with a greater sequence
// number) and that were grabbed or have changed since the
// last leave.
extern const char*        kMsgCLeave;

// grab clipboard:  primary <-> secondary
// sent by screen when some other app on that screen grabs a
// clipboard.  $1 = the clipboard identifier, $2 = sequence number.
// secondary screens must use the sequence number passed in the
// most recent kMsgCEnter.  the primary always sends 0.
extern const char*        kMsgCClipboard;

// screensaver change:  primary -> secondary
// screensaver on primary has started ($1 == 1) or closed ($1 == 0)
extern const char*        kMsgCScreenSaver;

// reset options:  primary -> secondary
// client should reset all of its options to their defaults.
extern const char*        kMsgCResetOptions;

// resolution change acknowledgment:  primary -> secondary
// sent by primary in response to a secondary screen's kMsgDInfo.
// this is sent for every kMsgDInfo, whether or not the primary
// had sent a kMsgQInfo.
extern const char*        kMsgCInfoAck;

// keep connection alive:  primary <-> secondary
// sent by the server periodically to verify that connections are still
// up and running.  clients must reply in kind on receipt.  if the server
// gets an error sending the message or does not receive a reply within
// a reasonable time then the server disconnects the client.  if the
// client doesn't receive these (or any message) periodically then it
// should disconnect from the server.  the appropriate interval is
// defined by an option.
extern const char*        kMsgCKeepAlive;

// offer a one-time token for a secondary bulk connection; primary -> secondary
extern const char*        kMsgCBulkOffer;

// offer a protocol 1.12 bulk connection; primary -> secondary
// $1 = one-time token, $2 = stable control-connection binding.
extern const char*        kMsgCBulkOffer1_12;

// accept/reject a secondary bulk connection; primary -> secondary
extern const char*        kMsgDBulkAccepted;
extern const char*        kMsgDBulkRejected;

// independent liveness probe/acknowledgment on the secondary bulk stream
extern const char*        kMsgBulkKeepAlive;
extern const char*        kMsgBulkKeepAliveAck;

//
// data codes
//

// key pressed:  primary -> secondary
// $1 = KeyID, $2 = KeyModifierMask, $3 = KeyButton
// the KeyButton identifies the physical key on the primary used to
// generate this key.  the secondary should note the KeyButton along
// with the physical key it uses to generate the key press.  on
// release, the secondary can then use the primary's KeyButton to
// find its corresponding physical key and release it.  this is
// necessary because the KeyID on release may not be the KeyID of
// the press.  this can happen with combining (dead) keys or if
// the keyboard layouts are not identical and the user releases
// a modifier key before releasing the modified key.
extern const char*        kMsgDKeyDown;

// key pressed 1.0:  same as above but without KeyButton
extern const char*        kMsgDKeyDown1_0;

// key auto-repeat:  primary -> secondary
// $1 = KeyID, $2 = KeyModifierMask, $3 = number of repeats, $4 = KeyButton
extern const char*        kMsgDKeyRepeat;

// key auto-repeat 1.0:  same as above but without KeyButton
extern const char*        kMsgDKeyRepeat1_0;

// key released:  primary -> secondary
// $1 = KeyID, $2 = KeyModifierMask, $3 = KeyButton
extern const char*        kMsgDKeyUp;

// key released 1.0:  same as above but without KeyButton
extern const char*        kMsgDKeyUp1_0;

// mouse button pressed:  primary -> secondary
// $1 = ButtonID
extern const char*        kMsgDMouseDown;

// mouse button released:  primary -> secondary
// $1 = ButtonID
extern const char*        kMsgDMouseUp;

// mouse moved:  primary -> secondary
// $1 = x, $2 = y.  x,y are absolute screen coordinates.
extern const char*        kMsgDMouseMove;

// relative mouse move:  primary -> secondary
// $1 = dx, $2 = dy.  dx,dy are motion deltas.
extern const char*        kMsgDMouseRelMove;

// mouse scroll:  primary -> secondary
// $1 = xDelta, $2 = yDelta.  the delta should be +120 for one tick forward
// (away from the user) or right and -120 for one tick backward (toward
// the user) or left.
extern const char*        kMsgDMouseWheel;

// mouse vertical scroll:  primary -> secondary
// like as kMsgDMouseWheel except only sends $1 = yDelta.
extern const char*        kMsgDMouseWheel1_0;

// Protocol 1.8 input frames. Each carries $1 = InputEpoch and
// $2 = InputSequence before the legacy input payload. Keyboard frames also
// carry $3 = EInputMessageFlags so broadcast input remains explicit.
extern const char*        kMsgDKeyDown1_8;
extern const char*        kMsgDKeyRepeat1_8;
extern const char*        kMsgDKeyUp1_8;
extern const char*        kMsgDMouseDown1_8;
extern const char*        kMsgDMouseUp1_8;
extern const char*        kMsgDMouseMove1_8;
extern const char*        kMsgDMouseRelMove1_8;
extern const char*        kMsgDMouseWheel1_8;

// response to kMsgCPrepareEnter: secondary -> primary.  A secondary that
// becomes unavailable between prepare and commit sends a second rejection for
// the same sequence. Protocol 1.10 sends a second positive response only after
// the input backend has committed the lease. $1 = sequence number, $2 = 1 when
// ready/committed and 0 when rejected.
extern const char*        kMsgDEnterReady;

// response to kMsgCRevokeInput: secondary -> primary.
// $1 = handoff sequence, $2 = 1 when the matching lease is inactive.
extern const char*        kMsgDRevokeInputAck;

// clipboard data:  primary <-> secondary
// $2 = sequence number, $3 = mark $4 = clipboard data.  the sequence number
// is 0 when sent by the primary.  secondary screens should use the
// sequence number from the most recent kMsgCEnter.  $1 = clipboard
// identifier.
extern const char*        kMsgDClipboard;

// client data:  secondary -> primary
// $1 = coordinate of leftmost pixel on secondary screen,
// $2 = coordinate of topmost pixel on secondary screen,
// $3 = width of secondary screen in pixels,
// $4 = height of secondary screen in pixels,
// $5 = size of warp zone, (obsolete)
// $6, $7 = the x,y position of the mouse on the secondary screen.
//
// the secondary screen must send this message in response to the
// kMsgQInfo message.  it must also send this message when the
// screen's resolution changes.  in this case, the secondary screen
// should ignore any kMsgDMouseMove messages until it receives a
// kMsgCInfoAck in order to prevent attempts to move the mouse off
// the new screen area.
extern const char*        kMsgDInfo;

// set options:  primary -> secondary
// client should set the given option/value pairs.  $1 = option/value
// pairs.
extern const char*        kMsgDSetOptions;

// file data:  primary <-> secondary
// transfer file data. A mark is used in the first byte.
// 0 means the content followed is the file size.
// 1 means the content followed is the chunk data.
// 2 means the file transfer is finished.
extern const char*        kMsgDFileTransfer;

// Protocol 1.12 transactional file transfer. Every frame carries a 128-bit
// control-connection binding and a role-qualified transfer identifier.
extern const char*        kMsgDFileTransferStart1_12;
extern const char*        kMsgDFileTransferStartAck1_12;
extern const char*        kMsgDFileTransferData1_12;
extern const char*        kMsgDFileTransferEnd1_12;
extern const char*        kMsgDFileTransferCancel1_12;
extern const char*        kMsgDFileTransferCancelAck1_12;
extern const char*        kMsgDFileTransferCommitAck1_12;

// drag information:  primary <-> secondary
// transfer drag information. The first 2 bytes are used for storing
// the number of dragging objects. Then the following string consists
// of each object's directory.
extern const char*        kMsgDDragInfo;

//
// query codes
//

// query screen info:  primary -> secondary
// client should reply with a kMsgDInfo.
extern const char*        kMsgQInfo;


//
// error codes
//

// incompatible versions:  primary -> secondary
// $1 = major version of primary, $2 = minor version of primary.
extern const char*        kMsgEIncompatible;

// name provided when connecting is already in use:  primary -> secondary
extern const char*        kMsgEBusy;

// unknown client:  primary -> secondary
// name provided when connecting is not in primary's screen
// configuration map.
extern const char*        kMsgEUnknown;

// protocol violation:  primary -> secondary
// primary should disconnect after sending this message.
extern const char*        kMsgEBad;

//! Validate the canonical 128-bit control-connection binding encoding.
bool isValidConnectionBinding(const std::string& binding);


//
// structures
//

//! Screen information
/*!
This class contains information about a screen.
*/
class ClientInfo {
public:
    //! Screen position
    /*!
    The position of the upper-left corner of the screen.  This is
    typically 0,0.
    */
    SInt32                m_x, m_y;

    //! Screen size
    /*!
    The size of the screen in pixels.
    */
    SInt32                m_w, m_h;

    //! Obsolete (jump zone size)
    SInt32                obsolete1;

    //! Mouse position
    /*!
    The current location of the mouse cursor.
    */
    SInt32                m_mx, m_my;
};
