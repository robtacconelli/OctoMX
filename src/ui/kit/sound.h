// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX UI: [ui-h] alert sounds (private message, Hot List user online, download complete).
//
// What is played comes from the settings (choiceFor): nothing, "beep" (the system beep), "sound"
// (a short chime built into the program) or the path of a .wav file. Playing never blocks:
//   * Windows: MessageBeep for the beep, PlaySound (asynchronous) for the chime (from memory) and
//     for a .wav file; nothing else is needed.
//   * Linux: the desktop's event sound player when one is installed (canberra-gtk-play, paplay,
//     pw-play or aplay, in this order) plays the chime and .wav files, canberra-gtk-play also the
//     "bell" event for the beep; without any of them the beep is QApplication::beep() and the chime
//     and .wav files fall back to it (Settings says so).
//
// Tests and screenshots never reach the speakers: a hook (setHook) or MX_SOUND_LOG=<file> records
// what would be played instead, and nothing is played at all on the offscreen / minimal platforms
// or when MX_DEMO_FROZEN or MX_SOUND_OFF is set (lastPlayed() still tells what it would have been).
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>

namespace mx {
class Settings;
}

namespace mx::ui::sound {

enum class Event { Message, HotlistOnline, DownloadDone };

// "" (nothing), "beep", "sound" or a .wav path, as the settings say for this event:
//   misc/sounds off: nothing at all;
//   Message: messages/soundOnMessage and messages/alertMode (quiet / beep / sound / wav + alertWav);
//   HotlistOnline: hotlist/soundOnline, then the same alert as for messages;
//   DownloadDone: misc/soundDownloadComplete, the built-in chime.
// A .wav that does not exist any more falls back to the built-in chime.
QString choiceFor(const Settings *s, Event e);

// Plays what choiceFor() says (asynchronously); returns it ("" = nothing played).
QString alert(const Settings *s, Event e);

// Plays "beep", "sound" or a .wav path (asynchronously); false if nothing could be started.
bool play(const QString &what);

// Recording instead of playing (tests). An empty hook restores normal playing.
using Hook = std::function<void(const QString &what)>;
void setHook(const Hook &h);
// What the last play() would have played (also when it was suppressed or recorded)
QString lastPlayed();

// The built-in chime as a complete RIFF/WAVE file (16-bit mono PCM, about 0.2 s)
QByteArray builtinWav();

// Linux: the program used for the chime and .wav files ("" = none found: they fall back to the beep).
// Windows: "PlaySound".
QString player();
// true when a .wav file can be played on this computer
bool canPlayFiles();

// true when nothing reaches the speakers in this process (tests, screenshots, offscreen)
bool suppressed();

} // namespace mx::ui::sound
