"""Профиль PGO со Switch по FTP (ftpd на консоли): *.gcda из /switch/shrek2/pgo -> build/pgo (AGENTS.md, «PGO»).

    python3 -I tools/pgo_fetch.py <SWITCH_IP> [--port 5000] [--out build/pgo] [--remote /switch/shrek2/pgo]
    SNX_PGO=use tools/build.sh switch

Каталог out очищается от старых *.gcda; в out/stamp — время забора (build.sh по нему пересобирает с нуля).
"""
import argparse
import ftplib
import glob
import os
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "pgo"))
    ap.add_argument("--remote", default="/switch/shrek2/pgo")
    args = ap.parse_args()
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=30)
    ftp.login()
    ftp.cwd(args.remote)  # ftpd на консоли: NLST с путём-аргументом отдаёт пустой список
    names = [n.rsplit("/", 1)[-1] for n in ftp.nlst()]
    names = [n for n in names if n.endswith(".gcda")]
    if not names:
        raise SystemExit(f"no *.gcda in {args.remote}")
    os.makedirs(args.out, exist_ok=True)
    for old in glob.glob(os.path.join(args.out, "*.gcda")):
        os.remove(old)
    total = 0
    for n in names:
        dst = os.path.join(args.out, n)
        with open(dst, "wb") as fp:
            ftp.retrbinary(f"RETR {n}", fp.write)
        total += os.path.getsize(dst)
    ftp.quit()
    with open(os.path.join(args.out, "stamp"), "w") as fp:
        fp.write(time.strftime("%Y-%m-%d %H:%M:%S") + f" {len(names)} files\n")
    print(f"{len(names)} files, {total >> 10} KB -> {args.out}")


if __name__ == "__main__":
    main()
