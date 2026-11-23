// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/chat_commands.h"

namespace mx {

ChatCommand parseChatCommand(const QString &input)
{
    ChatCommand c;
    QString line = input;
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
    if (line.trimmed().isEmpty()) return c;
    if (line.startsWith(QLatin1String("//"))) {
        c.type = ChatCommand::Text;
        c.text = line.mid(1);
        return c;
    }
    if (!line.startsWith(QLatin1Char('/'))) {
        c.type = ChatCommand::Text;
        c.text = line;
        return c;
    }
    const int sp = line.indexOf(QLatin1Char(' '));
    const QString word = (sp < 0 ? line.mid(1) : line.mid(1, sp - 1)).toLower();
    const QString rest = sp < 0 ? QString() : line.mid(sp + 1).trimmed();
    auto firstWord = [&rest](QString *remainder) {
        const int s = rest.indexOf(QLatin1Char(' '));
        if (remainder) *remainder = s < 0 ? QString() : rest.mid(s + 1).trimmed();
        return s < 0 ? rest : rest.left(s);
    };
    c.name = word;
    if (word == QLatin1String("me") || word == QLatin1String("action") || word == QLatin1String("emote")) {
        c.type = ChatCommand::Me;
        c.text = rest;
    } else if (word == QLatin1String("topic")) {
        c.type = ChatCommand::Topic;
        c.text = rest;
    } else if (word == QLatin1String("join") || word == QLatin1String("j")) {
        c.type = ChatCommand::Join;
        c.target = rest;
    } else if (word == QLatin1String("part") || word == QLatin1String("leave")) {
        c.type = ChatCommand::Part;
        c.target = rest;
    } else if (word == QLatin1String("msg") || word == QLatin1String("m") || word == QLatin1String("pm")
               || word == QLatin1String("query") || word == QLatin1String("privmsg")) {
        c.type = ChatCommand::Msg;
        c.target = firstWord(&c.text);
    } else if (word == QLatin1String("ignore")) {
        c.type = ChatCommand::Ignore;
        c.target = firstWord(nullptr);
    } else if (word == QLatin1String("unignore")) {
        c.type = ChatCommand::Unignore;
        c.target = firstWord(nullptr);
    } else if (word == QLatin1String("whois") || word == QLatin1String("w")) {
        c.type = ChatCommand::Whois;
        c.target = firstWord(nullptr);
    } else if (word == QLatin1String("clear") || word == QLatin1String("cls")) {
        c.type = ChatCommand::Clear;
    } else if (word == QLatin1String("help") || word == QLatin1String("?")) {
        c.type = ChatCommand::Help;
    } else if (word == QLatin1String("motd")) {
        c.type = ChatCommand::Motd;
    } else if (word == QLatin1String("setmotd")) {
        c.type = ChatCommand::SetMotd;
        c.text = rest;
        c.text.replace(QLatin1String("\\n"), QLatin1String("\n"));
        c.text.replace(QLatin1String(" | "), QLatin1String("\n"));
    } else {
        c.type = ChatCommand::Unknown;
    }
    return c;
}

QStringList chatHelpLines()
{
    return {
        QStringLiteral("Available commands:"),
        QStringLiteral("  /me <action>          - describe what you are doing"),
        QStringLiteral("  /topic [new topic]    - show or change the room topic"),
        QStringLiteral("  /motd                 - show the room's message of the day"),
        QStringLiteral("  /setmotd <text>       - change it (\\n or \" | \" starts a new line)"),
        QStringLiteral("  /join <room>          - join (or create) a room"),
        QStringLiteral("  /part [room]          - leave the current (or named) room"),
        QStringLiteral("  /msg <user> <text>    - send a private message"),
        QStringLiteral("  /ignore <user>        - ignore a user (/unignore to undo)"),
        QStringLiteral("  /whois <user>         - show information about a user"),
        QStringLiteral("  /clear                - clear this window"),
        QStringLiteral("  /help                 - this list"),
        QStringLiteral("  //text                - send text starting with a slash"),
    };
}

} // namespace mx
