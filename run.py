import glob, os
from PIL import Image

BACKUP = os.path.expanduser("~/clawd")
CLAWD  = "/home/moro/source/claude-desktop-buddy-waveshare-esp32s3-touch/characters/clawd"
TW, TH  = 120, 120
BG      = (40, 40, 40)   # #282828

def crop_box(w, h):
    l = round(w * 8 / 38);  r = round(w * 8 / 38)
    t = round(h * 17 / 38); b = round(h * 2 / 38)
    return (l, t, w - r, h - b)

def process(src, dst):
    img = Image.open(src)
    frames, durations = [], []

    for i in range(getattr(img, 'n_frames', 1)):
        img.seek(i)
        dur = img.info.get('duration', 100)

        # Flatten frame over solid #282828 — no transparency needed since pal.bg matches
        bg = Image.new('RGB', img.size, BG)
        frame = img.convert('RGBA')
        bg.paste(frame, mask=frame.split()[3])   # composite over bg

        bg = bg.crop(crop_box(*bg.size))
        bg = bg.resize((TW, TH), Image.LANCZOS)

        # Quantize RGB (no alpha complications)
        p = bg.quantize(colors=256, method=Image.Quantize.MEDIANCUT,
dither=Image.Dither.NONE)
        frames.append(p)
        durations.append(dur)

    frames[0].save(
        dst, save_all=True, append_images=frames[1:],
        loop=0, duration=durations, disposal=2, optimize=False
    )
    size = os.path.getsize(dst) // 1024
    print(f"  {os.path.basename(dst)}: {len(frames)} frames  {size}KB")

for src in sorted(glob.glob(f"{BACKUP}/*.gif")):
    name = os.path.basename(src)
    try:
        process(src, os.path.join(CLAWD, name))
    except Exception as e:
        print(f"  FAIL {name}: {e}")