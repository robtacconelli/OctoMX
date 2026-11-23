#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-g] Right clicks with a real mouse (xdotool on an X display, also under wine) on the rows of every
# list, at the top edge, the middle and the bottom edge of a row, on text and on the empty right end;
# the app logs (script verb "watch") which row each menu was opened for, the menu entries with their
# screen positions, clipboard changes and question boxes. The driver then picks "Copy Name" and checks
# the clipboard holds the clicked row's name; once per page it tries a destructive entry (Cancel,
# Delete File, Remove) and checks the question names the clicked row (the watcher answers No).
#
#   python3 -I tests/ui/rightclick_real_mouse.py --exe <octomx or octomx.exe> [--wine] --display :N
#           --scale 100 --work <dir> [--pages transfers,search,shared,hotlist,chat]
# Prints a summary and writes <work>/report-<scale>.txt. Exit 1 when a click acted on another row.
import argparse
import os
import shutil
import subprocess
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument('--exe', required=True)
ap.add_argument('--wine', action='store_true')
ap.add_argument('--display', required=True)
ap.add_argument('--scale', type=int, default=100)
ap.add_argument('--work', required=True)
ap.add_argument('--xdotool', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xtest_input.py'))
ap.add_argument('--pages', default='transfers,search,shared,hotlist,chat')
args = ap.parse_args()

work = os.path.abspath(args.work)
os.makedirs(work, exist_ok=True)
env = dict(os.environ)
env['DISPLAY'] = args.display
env['LC_ALL'] = 'C.UTF-8'
env['MX_DIGIT_GROUPING'] = 'space'
env['MX_UI_SCALE'] = str(args.scale)
env.pop('QT_QPA_PLATFORM', None)


def winpath(p):
    if not args.wine:
        return p
    return subprocess.run(['winepath', '-w', p], capture_output=True, text=True, env=env).stdout.strip()


def xdo(*a):
    tool = [sys.executable, '-I', args.xdotool] if args.xdotool.endswith('.py') else [args.xdotool]
    subprocess.run(tool + [str(x) for x in a], env=env, check=False, capture_output=True)


PAGES = {
    'transfers': (['page transfers', 'wait 1500'], ['downloadList', 'uploadList']),
    'search': (['page search', 'type searchText linux', 'key searchText Return', 'wait 1500',
                'expand-row searchResults 0', 'wait 300'], ['searchResults']),
    'shared': (['page shared', 'wait 800', 'expand-row sharedList 0', 'wait 300'], ['sharedList']),
    'hotlist': (['page transfers', 'hotlist on', 'wait 800'], ['hotlistList']),
    'chat': (['page chat', 'join Linux Users', 'wait 1500'], ['chatUserList']),
}
DESTRUCTIVE = {'downloadList': 'Cancel', 'uploadList': None, 'searchResults': None, 'sharedList': None,
               'hotlistList': 'Remove', 'chatUserList': None}


def read_lines(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def wait_new(path, start, prefix, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        lines = read_lines(path)
        for ln in lines[start:]:
            if ln.startswith(prefix):
                return ln, len(lines)
        time.sleep(0.05)
    return None, len(read_lines(path))


def entries_of(menu_line):
    # "menu <name> Text@x,y;Text@x,y"
    out = {}
    body = menu_line.split(' ', 2)[2] if menu_line.count(' ') >= 2 else ''
    for e in body.split(';'):
        if '@' not in e:
            continue
        text, pos = e.rsplit('@', 1)
        try:
            x, y = pos.split(',')
            out[text] = (int(x), int(y))
        except ValueError:
            pass
    return out


report = []
failures = 0
total = 0
for page in [p for p in args.pages.split(',') if p]:
    setup, lists = PAGES[page]
    d = os.path.join(work, f'{page}-{args.scale}')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, 'prof'))
    with open(os.path.join(d, 'prof', 'settings.json'), 'w') as f:
        f.write('{ "ui/hotlistConfirmRemove": true, "transfers/confirmCancel": true, "ui/chatWarningDisabled": true }\n')
    ready, stop, watch = (os.path.join(d, n) for n in ('READY', 'STOP', 'watch.log'))
    rows = {l: os.path.join(d, f'rows-{l}.txt') for l in lists}
    script = setup + [f'report-rows {l} {winpath(rows[l])}' for l in lists] + \
        [f'echo-file {winpath(ready)}', f'watch 900 {winpath(watch)} {winpath(stop)}', 'wait 900000', 'quit']
    with open(os.path.join(d, 'a.script'), 'w') as f:
        f.write('\n'.join(script) + '\n')
    cmd = (['wine'] if args.wine else []) + [args.exe, '--profile', winpath(os.path.join(d, 'prof')), '--demo-frozen',
                                             '--size', '1000x700', '--script', winpath(os.path.join(d, 'a.script'))]
    log = open(os.path.join(d, 'app.log'), 'w')
    proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    t0 = time.time()
    while not os.path.exists(ready) and time.time() - t0 < 120 and proc.poll() is None:
        time.sleep(0.2)
    if not os.path.exists(ready):
        report.append(f'{page}: the app did not get ready (see {d}/app.log)')
        failures += 1
        proc.kill()
        continue
    time.sleep(0.5)
    for lst in lists:
        rws = []
        for ln in read_lines(rows[lst]):
            p = ln.split(' ', 6)
            if len(p) >= 6:
                rws.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5]), p[6] if len(p) > 6 else ''))
        if not rws:
            report.append(f'{page}/{lst}: no rows')
            continue
        pick = sorted({0, min(1, len(rws) - 1), len(rws) // 2, len(rws) - 1})
        ok = copied = nomenu = 0
        bad = []
        for k in pick:
            n, x, y, w, h, text = rws[k]
            for (px, where) in ((x + 40, 'text'), (x + w - 6, 'right end')):
                for (py, edge) in ((y + 1, 'top'), (y + h // 2, 'middle'), (y + h - 2, 'bottom')):
                    total += 1
                    start = len(read_lines(watch))
                    xdo('mousemove', px, py)
                    time.sleep(0.05)
                    xdo('click', 3)
                    ctx, start2 = wait_new(watch, start, 'context ')
                    if not ctx:
                        bad.append(f'row {n} {edge}/{where}: no menu event')
                        continue
                    got = ctx.split(' row=', 1)[1].split(' current=', 1)[0] if ' row=' in ctx else '?'
                    cur = ctx.split(' current=', 1)[1].split(' selected=', 1)[0] if ' current=' in ctx else '?'
                    if got != text or cur != text:
                        bad.append(f'row {n} "{text}" {edge}/{where}: menu for "{got}", highlighted "{cur}"')
                    else:
                        ok += 1
                    menu, _ = wait_new(watch, start, 'menu ', 1.5)
                    if not menu:
                        nomenu += 1
                        xdo('key', 'Escape')
                        time.sleep(0.2)
                        continue
                    ent = entries_of(menu)
                    if 'Copy Name' in ent:
                        before = len(read_lines(watch))
                        cx, cy = ent['Copy Name']
                        xdo('mousemove', cx, cy)
                        time.sleep(0.1)
                        xdo('click', 1)
                        clip, _ = wait_new(watch, before, 'clipboard ', 2.0)
                        if clip and clip[len('clipboard '):] == text:
                            copied += 1
                        elif clip:
                            bad.append(f'row {n} "{text}" {edge}/{where}: Copy Name gave "{clip[10:]}"')
                        else:
                            bad.append(f'row {n} "{text}" {edge}/{where}: Copy Name changed nothing')
                    else:
                        xdo('key', 'Escape')
                    time.sleep(0.25)
        failures += len(bad)
        report.append(f'{page}/{lst} at {args.scale} %: {ok} of {len(pick) * 6} right clicks opened the menu of the '
                      f'clicked row, Copy Name copied the clicked row {copied} times, {nomenu} rows without a menu '
                      f'(section titles)')
        report.extend('    FAIL ' + b for b in bad)
        # a destructive entry names the clicked row
        entry = DESTRUCTIVE.get(lst)
        if entry:
            target = next((r for r in rws if r[5] and not r[5].startswith(('Online', 'Offline', 'Temporary'))), None)
            if lst == 'downloadList':   # a running download (file row) for Cancel
                target = next((r for r in rws if 'Velorine - The Best Of Velorine' in r[5]), target)
            if target:
                n, x, y, w, h, text = target
                start = len(read_lines(watch))
                xdo('mousemove', x + 40, y + h // 2)
                xdo('click', 3)
                menu, _ = wait_new(watch, start, 'menu ', 3)
                ent = entries_of(menu) if menu else {}
                if entry in ent:
                    xdo('mousemove', *ent[entry])
                    time.sleep(0.1)
                    xdo('click', 1)
                    q, _ = wait_new(watch, start, 'question ', 3)
                    # the file (before " - " of a music name) or the user (a Hotlist row adds its network)
                    key = text.split(' - ')[0] if lst != 'hotlistList' else text.split(' ')[0]
                    named = bool(q) and key in q
                    report.append(f'    {entry} on "{text}": question {"names the row" if named else "DOES NOT name the row"}: '
                                  f'{q[9:] if q else "(no question)"}')
                    failures += 0 if named else 1
                else:
                    report.append(f'    {entry} not in the menu of "{text}"')
                    xdo('key', 'Escape')
                time.sleep(0.3)
    open(stop, 'w').close()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()

with open(os.path.join(work, f'report-{args.scale}.txt'), 'w') as f:
    f.write('\n'.join(report) + '\n')
print('\n'.join(report))
print(f'{total} right clicks, {failures} problems')
sys.exit(1 if failures else 0)
