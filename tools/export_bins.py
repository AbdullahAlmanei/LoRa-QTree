# tools/export_bins.py
import os, shutil
from SCons.Script import Import

Import("env")

def export_bins(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir   = env.subst("$BUILD_DIR")
    env_name    = os.path.basename(build_dir)
    out_dir     = os.path.join(project_dir, "docs", env_name)
    os.makedirs(out_dir, exist_ok=True)

    def copy_named(src_path, dst_name):
        if os.path.exists(src_path):
            shutil.copy2(src_path, os.path.join(out_dir, dst_name))
            print(f"[export_bins] {env_name}: copied {dst_name} -> docs/{env_name}/")
            return True
        return False

    fw  = os.path.join(build_dir, "firmware.bin")
    bl  = os.path.join(build_dir, "bootloader.bin")
    pts = os.path.join(build_dir, "partitions.bin")

    if not copy_named(fw, "firmware.bin"):
        print(f"[export_bins] {env_name}: WARN missing firmware.bin")

    if not copy_named(bl, "bootloader.bin"):
        alt_bl = next((os.path.join(build_dir, f)
                       for f in os.listdir(build_dir)
                       if f.startswith("bootloader") and f.endswith(".bin")),
                      None)
        if alt_bl and copy_named(alt_bl, "bootloader.bin"):
            pass
        else:
            print(f"[export_bins] {env_name}: WARN missing bootloader.bin")

    if not copy_named(pts, "partitions.bin"):
        alt_pt = next((os.path.join(build_dir, f)
                       for f in os.listdir(build_dir)
                       if f.startswith("partitions") and f.endswith(".bin")),
                      None)
        if alt_pt and copy_named(alt_pt, "partitions.bin"):
            pass
        else:
            print(f"[export_bins] {env_name}: WARN missing partitions.bin")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export_bins)
