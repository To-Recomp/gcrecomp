#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Ported from the author's own fepor-recomp (tools/dolphin-run.sh), same author.
# Run the game in Dolphin (the reference emulator) in an isolated user directory, driven through Dolphin's
# named-pipe controller, and take screenshots: for comparing saves, memory cards, frames and audio with the recomp.
#
# Usage: tools/dolphin-run.sh --iso <disc> [--card <raw> | --gci <save.gci>] [--user <dir>] -- <step>...
# Steps run in order: wait:<seconds>  press:<A|B|X|Y|Z|START|L|R|D_UP|D_DOWN|D_LEFT|D_RIGHT>  mash:<button>:<seconds>  stick:<dir>[+<dir>][:<seconds>]  shot:<file.png>
# --card <raw>: raw image as slot A (Dolphin may write to it: pass a copy). --gci <file>: a GCI-folder card holding that one save.
# --dump-audio: Dolphin writes its audio to <user>/Dump/Audio/*dspdump.wav (compare with GCRECOMP_DUMP_AUDIO using tools/wav-compare.py).
# --dump-frames: Dolphin writes the video to <user>/Dump/Frames/*.avi (extract frames with ffmpeg to see what was on screen).
# --no-audio: no audio output (the game's sound is not played on the speakers).
# --headless: no window and no X server needed (Qt's offscreen platform; the Software renderer unless --backend says
#   another: OGL, Vulkan, Software, Null); the video is dumped as PNG frames and `shot:<file.png>` copies the newest dumped
#   frame instead of taking a desktop screenshot (no spectacle needed). The frames are deleted at the end unless
#   --dump-frames is also given. Runs in the background without disturbing the desktop.
# --iso takes an image; tools/gcm-from-dir.py rebuilds one from an extracted disc directory (Dolphin's folder boot does not
#   accept the extractor's layout).
# --play: play by hand instead of by script: no steps, the window stays open until you close it (or Ctrl-C). Your own
#   controller/keyboard mapping is copied from --pad-ini FILE (default ~/.config/dolphin-emu/GCPadNew.ini if it exists)
#   into the isolated user dir, so the real configuration is never touched. Combine with --card (a COPY of a card image),
#   --dump-frames and --dump-audio to keep what was on screen for comparison with the recomp.
# Needs dolphin-emu, spectacle (KDE) and ImageMagick for the screenshots. Linux only.
set -euo pipefail
iso=""; card=""; gci=""; user="/tmp/dolphin-run-user"; dump_audio=0; dump_frames=0; mute=0; headless=0; backend=""; play=0; padini=""
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) iso="$2"; shift 2 ;;
    --card) card="$2"; shift 2 ;;
    --gci) gci="$2"; shift 2 ;;
    --user) user="$2"; shift 2 ;;
    --dump-audio) dump_audio=1; shift ;;
    --dump-frames) dump_frames=1; shift ;;
    --no-audio) mute=1; shift ;;
    --headless) headless=1; shift ;;
    --play) play=1; shift ;;
    --pad-ini) padini="$2"; shift 2 ;;
    --backend) backend="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$iso" ] || { echo "--iso is required" >&2; exit 2; }
if [ -n "$card" ] && [ -n "$gci" ]; then echo "use --card or --gci, not both" >&2; exit 2; fi

rm -rf "$user"; mkdir -p "$user/Config" "$user/Pipes"
slot=255
if [ -n "$card" ]; then slot=1; fi
if [ -n "$gci" ]; then
  # a GCI folder card (Dolphin's default): the saves are loose .gci files
  slot=8
  mkdir -p "$user/GC/EUR/Card A"
  cp "$gci" "$user/GC/EUR/Card A/"
fi
cat > "$user/Config/Dolphin.ini" <<EOF
[Core]
SkipIPL = True
SlotA = $slot
SlotB = 255
$([ -n "$card" ] && echo "MemcardAPath = $card")
[DSP]
DumpAudio = $([ "$dump_audio" = 1 ] && echo True || echo False)
$([ "$mute" = 1 ] && echo "Backend = No Audio Output")
[Interface]
ConfirmStop = False
EOF
# [Movie]: dump the video (lossless AVI) so the scene at each moment can be checked offline
if [ "$dump_frames" = 1 ]; then
  printf '[Movie]\nDumpFrames = True\nDumpFramesSilent = True\n' >> "$user/Config/Dolphin.ini"
fi
if [ "$play" = 1 ]; then
  [ -n "$padini" ] || padini="$HOME/.config/dolphin-emu/GCPadNew.ini"
  if [ -f "$padini" ]; then cp "$padini" "$user/Config/GCPadNew.ini"; else echo "[dolphin-run] no pad config ($padini): Dolphin's defaults apply" >&2; fi
else
cat > "$user/Config/GCPadNew.ini" <<'EOF'
[GCPad1]
Device = Pipe/0/pad
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
Triggers/L = `Button L`
Triggers/R = `Button R`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
Main Stick/Up = `Axis Y +`
Main Stick/Down = `Axis Y -`
Main Stick/Left = `Axis X -`
Main Stick/Right = `Axis X +`
EOF
mkfifo "$user/Pipes/pad"
fi

# headless: dump the frames (shots copy the newest one)
if [ "$headless" = 1 ] && [ "$dump_frames" = 0 ]; then
  printf '[Movie]\nDumpFrames = True\nDumpFramesSilent = True\n' >> "$user/Config/Dolphin.ini"
fi
if [ "$headless" = 1 ]; then export QT_QPA_PLATFORM=offscreen; [ -n "$backend" ] || backend=Software; fi
dolphin_args=(-u "$user" -b)
[ -z "$backend" ] || dolphin_args+=(-v "$backend")
# Own session and process group: the cleanup kills that group (the launcher may start a child), never a process found by name.
setsid dolphin-emu "${dolphin_args[@]}" -e "$iso" > "$user/dolphin.log" 2>&1 &
dolphin=$!
cleanup() {
  kill -- "-$dolphin" 2>/dev/null || true
  wait "$dolphin" 2>/dev/null || true
  if [ "$headless" = 1 ] && [ "$dump_frames" = 0 ]; then
    sleep 2
    rm -rf "$user/Dump/Frames"
  fi
}
trap cleanup EXIT
if [ "$play" = 1 ]; then
  echo "[dolphin-run] Dolphin is running (user dir $user); close its window to stop."
  wait "$dolphin" || true
  exit 0
fi
exec 3<>"$user/Pipes/pad"
for step in "$@"; do
  case "$step" in
    wait:*) sleep "${step#wait:}" ;;
    press:*) echo "PRESS ${step#press:}" >&3; sleep 0.3; echo "RELEASE ${step#press:}" >&3 ;;
    stick:*)
      # tilt the main stick: stick:DIR[+DIR...][:SECONDS] with DIR in left|right|up|down (menus often read the stick,
      # not the D-pad); held 0.4 s unless SECONDS is given: stick:up+right:2.5 walks 2.5 s
      spec="${step#stick:}"; secs=0.4
      case "$spec" in *:*) secs="${spec#*:}"; spec="${spec%%:*}" ;; esac
      x=0.5; y=0.5
      IFS=+ read -ra dirs <<< "$spec"
      for d in "${dirs[@]}"; do
        case "$d" in
          left) x=0 ;; right) x=1 ;; up) y=1 ;; down) y=0 ;;
          *) echo "unknown stick direction $d" >&2; exit 2 ;;
        esac
      done
      echo "SET MAIN $x $y" >&3; sleep "$secs"; echo "SET MAIN 0.5 0.5" >&3 ;;
    mash:*)
      # press a button every 1.5 s for the given time: gets through screens whose timing varies
      IFS=: read -r _ btn secs <<< "$step"
      end=$((SECONDS + secs))
      while [ "$SECONDS" -lt "$end" ]; do
        echo "PRESS $btn" >&3; sleep 0.3; echo "RELEASE $btn" >&3; sleep 1.2
      done ;;
    shot:*)
      if [ "$headless" = 1 ]; then
        # the newest dumped frame (the file being written is the highest number: take the one before it)
        newest=$(ls "$user/Dump/Frames" | sed 's/[^0-9]//g' | sort -n | tail -2 | head -1)
        cp "$user/Dump/Frames/framedump_$newest.png" "${step#shot:}"
      else
        spectacle -b -n -f -o "${step#shot:}" > /dev/null 2>&1
      fi ;;
    *) echo "unknown step $step" >&2; exit 2 ;;
  esac
done
