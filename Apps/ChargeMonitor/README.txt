CHARGE MONITOR v1.3 - TrimUI Brick Pro (stock OS)

CAI DAT: giai nen, chep thu muc Apps/ChargeMonitor vao the nho:
  SD:/Apps/ChargeMonitor/   (gop thu muc neu duoc hoi)
Mo tu menu Apps -> "Charge Monitor", cam sac.

PHIM:
  START            tat man hinh ngay
  SELECT           bat/tat "tu tat may khi pin day"
  MENU / giu 2s    thoat (khoi phuc moi cai dat)
  Phim bat ky      bat lai man hinh
Man hinh tu tat sau 30 giay khong bam phim.

TOI UU KHI CHAY (tu khoi phuc khi thoat):
  - CPU ve xung thap nhat, tat nhan 1-3
  - Den nen giam ~12%, den LED RGB tat
  - Tat Wi-Fi (bat lai khi thoat)

LOG: logs/charge_*.csv (moi 30 giay), logs/run.log (thong tin chan doan,
ma phim). Neu phim khong dung, mo run.log xem dong "key NNN" va bao lai.

BUILD LAI: zig cc -target aarch64-linux-musl -O2 -static -o chargemon src/chargemon.c -lm
(chay src/gen_font.py truoc de tao font.h)
