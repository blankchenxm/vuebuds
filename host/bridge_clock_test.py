"""Bridge clock continuity (PR #28 review, sw-mac 11-20): Q with boards, 330 s without any board
(longer than half an RTC0 wrap), boards again; the bridge time must stay on one straight line
against the PC clock, and the bridge logs "clock check" (bridge time - newest anchor) per link."""
import sys, time, statistics as st
sys.path.insert(0, __import__("os").path.dirname(__file__))
import bridge, protocol
texts = []
pts = []
addrs = [protocol.BOARDS[b]["address"] for b in ("hm0360-5340", "hm0360b-5340")]
with bridge.Bridge(on_text=lambda s, t: texts.append((time.monotonic(), s, t)), on_event=lambda s, l: None) as br:
    t0 = time.monotonic()
    def sample(phase, n, every):
        for _ in range(n):
            c = br.bridge_clock(samples=8)
            if c: pts.append((time.monotonic() - t0, phase, c[0], c[1]))
            else: print("no answer", phase)
            time.sleep(every)
    br.configure(addrs); br.wait_ready([0, 1], timeout=30); time.sleep(1)
    sample("A boards", 10, 4)
    br.configure([]); print("boards dropped", round(time.monotonic() - t0), flush=True)
    sample("B none", 33, 10)
    t_c = time.monotonic(); br.configure(addrs)
    print("ready again:", br.wait_ready([0, 1], timeout=40, after=t_c), flush=True); time.sleep(1.5)
    sample("C boards", 10, 4)
t = [p[0] for p in pts]; o = [p[2] for p in pts]
mt, mo = st.mean(t), st.mean(o)
b = sum((x - mt) * (y - mo) for x, y in zip(t, o)) / sum((x - mt) ** 2 for x in t)
for ph in ("A boards", "B none", "C boards"):
    r = [p[2] - (mo + b * (p[0] - mt)) for p in pts if p[1] == ph]
    rt = [p[3] for p in pts if p[1] == ph]
    print(f"{ph}: n={len(r)}, residual vs one line mean {st.mean(r):.0f} us, max |{max(abs(x) for x in r):.0f}| us, rtt median {st.median(rt):.0f} us")
print(f"span {t[-1]-t[0]:.0f} s, slope {b:.2f} us/s")
for tt, s, x in texts:
    if "clock check" in x: print(f"  t={tt-t0:.0f}s slot {s}: {x}")
