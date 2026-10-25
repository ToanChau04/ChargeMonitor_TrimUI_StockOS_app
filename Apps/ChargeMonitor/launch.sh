#!/bin/sh
# ChargeMonitor - theo dõi sạc pin & tối ưu máy khi sạc (TrimUI Brick Pro, stock OS)
APPDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APPDIR" || exit 1
mkdir -p logs
chmod +x ./chargemon 2>/dev/null

# Tắt Wi-Fi trong lúc sạc (bật lại khi thoát nếu trước đó đang bật)
WIFI_WAS_UP=0
if [ -e /sys/class/net/wlan0/operstate ] && [ "$(cat /sys/class/net/wlan0/operstate)" = "up" ]; then
    WIFI_WAS_UP=1
    ifconfig wlan0 down 2>/dev/null && export CM_WIFI_OFF=1
fi

./chargemon > logs/run.log 2>&1
RC=$?

if [ "$WIFI_WAS_UP" = "1" ]; then
    ifconfig wlan0 up 2>/dev/null
fi

# Mã 10 = pin đã đầy và người dùng bật "tự tắt máy khi đầy"
if [ "$RC" = "10" ]; then
    sync
    poweroff
fi
exit 0
