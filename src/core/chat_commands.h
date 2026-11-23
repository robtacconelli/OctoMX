// SPDX-License-Identifier: GPL-3.0-or-later
// OctoMX core: parsing of chat input lines ("/me waves", "/msg Bob hi", "//literal").
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace mx {

struct ChatCommand {
    enum Type {
        Empty = 0,  // blank line
        Text,       // ordinary message (text)
        Me,         // /me <action>                (text)
        Topic,      // /topic [new topic]          (text; empty = show the topic)
        Join,       // /join <room>                (target)
        Part,       // /part [room]                (target; empty = current room)
        Msg,        // /msg <user> <text>          (target, text)
        Ignore,     // /ignore <user>              (target)
        Unignore,   // /unignore <user>            (target)
        Whois,      // /whois <user>               (target)
        Clear,      // /clear
        Help,       // /help
        Motd,       // /motd                       show the room's message of the day
        SetMotd,    // /setmotd <text>             (text; "\n" or " | " starts a new line)
        Unknown,    // anything else               (name = the command word)
    };
    Type type = Empty;
    QString target;
    QString text;
    QString name;
};

// "//text" sends "/text" literally. Command words are case-insensitive; aliases: /action /emote
// (me), /j (join), /leave (part), /m /pm /query /privmsg (msg), /w (whois), /cls (clear), /? (help).
ChatCommand parseChatCommand(const QString &input);
QStringList chatHelpLines();

} // namespace mx
