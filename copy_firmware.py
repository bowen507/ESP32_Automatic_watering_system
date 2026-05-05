import shutil
import os

Import("env")

def after_build(source, target, env):
    # Get the firmware path
    firmware_source = str(target[0])

    # Target directory (releases folder in project root)
    release_dir = os.path.join(env.subst("$PROJECT_DIR"), "releases")

    # Target file path
    firmware_dest = os.path.join(release_dir, "firmware.bin")

    print(f"Checking directory: {release_dir}")
    if not os.path.exists(release_dir):
        os.makedirs(release_dir)
        print("Created releases directory")

    print(f"Copying {firmware_source} -> {firmware_dest}")
    try:
        shutil.copy(firmware_source, firmware_dest)
        print("--> Firmware successfully copied to releases folder!")
    except Exception as e:
        print(f"--> Error copying firmware: {e}")

# Bind to the "bin" file generation
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)
