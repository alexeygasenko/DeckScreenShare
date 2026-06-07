#!/usr/bin/env bash
set -euo pipefail

export XDG_RUNTIME_DIR=/tmp/deck-screen-share-runtime
export ASAN_OPTIONS=detect_leaks=0
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

meson setup /tmp/agent-build /src/steamos -Db_sanitize=address,undefined >/dev/null
meson compile -C /tmp/agent-build >/dev/null

pipewire >/tmp/pipewire.log 2>&1 &
sleep 2
ln -sf "$XDG_RUNTIME_DIR/pipewire-0" "$XDG_RUNTIME_DIR/pipewire-0-manager"
dbus-run-session -- wireplumber >/tmp/wireplumber.log 2>&1 &
sleep 2

pipewire-pulse >/tmp/pipewire-pulse.log 2>&1 &
sleep 2
pactl load-module module-null-sink sink_name=deckshare >/dev/null

gst-launch-1.0 -q videotestsrc is-live=true pattern=ball \
  ! video/x-raw,format=NV12,width=1280,height=800,framerate=60/1 \
  ! pipewiresink client-name=gamescope mode=provide \
    stream-properties='props,node.name=gamescope,media.class=Video/Source' \
  >/tmp/source.log 2>&1 &

/tmp/agent-build/deck-screen-share-agent >/tmp/agent.out 2>/tmp/agent.err &
agent_pid=$!
sleep 2

body='{"receiver_host":"127.0.0.1","srt_port":9000,"codec":"h264","backend":"software","capture_mode":"pipewire","pipewire_pipeline":"cpu","fps":60,"video_bitrate_kbps":2000,"audio_bitrate_kbps":96,"latency_ms":120,"size":"1280x800","display":":0.0","audio_source":"deckshare.monitor","drm_device":"/dev/dri/card0","vaapi_device":"/dev/dri/renderD128"}'

for cycle in $(seq 1 15); do
  ffmpeg -hide_banner -loglevel error -y \
    -i 'udp://0.0.0.0:9000?fifo_size=1000000&overrun_nonfatal=1' \
    -t 5 -c copy "/tmp/cycle-$cycle.mkv" \
    >/tmp/receiver.log 2>&1 &
  receiver_pid=$!
  sleep 0.2
  curl -fsS -X POST -H 'Content-Type: application/json' \
    --data "$body" http://127.0.0.1:8765/api/start >/dev/null
  sleep 0.3
  link_count=$(pgrep -fc '^pw-link -L [0-9]+ [0-9]+$' || true)
  test "$link_count" -le 1
  curl -fsS http://127.0.0.1:8765/api/status >/dev/null
  curl -fsS -X POST -H 'Content-Type: application/json' \
    --data '{}' http://127.0.0.1:8765/api/stop >/dev/null
  kill "$receiver_pid" 2>/dev/null || true
  wait "$receiver_pid" 2>/dev/null || true
  sleep 0.3

  link_count=$(pgrep -fc '^pw-link -L [0-9]+ [0-9]+$' || true)
  test "$link_count" -eq 0
  kill -0 "$agent_pid"
done

curl -fsS http://127.0.0.1:8765/api/status
echo
echo "Completed 15 sanitized UDP start/stop cycles."
