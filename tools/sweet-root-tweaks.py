#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
turned the plain mkiosroot.sh output into the images that reach the home
screen (image versions v8-v14 in NOTES.md):

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); CommCenter jobs removed (no baseband: it
                  crash-loops and Setup blocks on its CoreTelephony XPC)
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark seeded; the actual Activated state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py
"""
import glob, os, plistlib, shutil, sys

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


def launchd(root):
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
    for k in removed:
        del jobs[k]
    # A job whose stdout/stderr is /dev/console cannot be spawned: xpcproxy
    # exits 78 (EX_CONFIG) because /dev/console is not openable for the mobile
    # user, and launchd just tries again.  That is the bare `exec xpcproxy`
    # loop that never reaches backboardd.  NOTES.md records the same finding
    # for v8 ("posix_spawn(/usr/libexec/backboardd), error 0xd - Permission
    # denied (xpcproxy exit 78) - /dev/console not openable") and the images
    # from v9 on rename the keys in place; an unknown key costs nothing, so
    # the rename is idempotent.  Daemon output still reaches the log through
    # patch 0048, which tees /dev/console.
    quiet = 0
    for job in jobs.values():
        for key in ('StandardOutPath', 'StandardErrorPath'):
            if key in job:
                job[key + 'X'] = job.pop(key)
                quiet += 1
    save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {renamed} _PanicOnCrash renamed, '
          f'{len(removed)} CommCenter jobs removed, '
          f'{quiet} stdout/stderr keys pointed away from /dev/console')


# Daemons that crash-loop on a bring-up kernel because a facility is missing:
# PosterBoard wants a Metal device, financed/healthdappd/driverkitd/rtcreportingd
# fault inside the same libSystem path, fairplayd.H2 and bluetoothd need SE and
# a UART respectively. None of them is needed to reach the home screen, and each
# relaunch burns CPU that the CPU-only renderer needs. Throttle them instead of
# removing them, so a later stage can simply drop the interval.
THROTTLE = ('PosterBoard', 'financed', 'healthdappd', 'driverkitd',
            'rtcreportingd', 'fairplayd', 'bluetoothd', 'symptomd', 'logd',
            'calaccessd', 'chronod', 'cloudd', 'ScreenTimeAgent',
            'mobileactivation', 'ReportCrash')

# Throttling only slows a crash loop down; while it runs, launchd keeps
# respawning the job and nothing else gets the CPU the software renderer
# needs. These ones are not needed to draw the lock screen or the home
# screen, so they are switched off outright. Delete a label from this
# list to bring the job back once its missing kernel feature exists.
DISABLE = ('calaccessd', 'chronod', 'cloudd', 'ScreenTimeAgent',
           'mobileactivation')


def throttle(root, interval=120):
    """Raise ThrottleInterval on the crash-looping jobs (idempotent)."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    hit = 0
    for job in plist['LaunchDaemons'].values():
        label = job.get('Label', '')
        if not any(name.lower() in label.lower() for name in THROTTLE):
            continue
        if job.get('ThrottleInterval') == interval:
            continue
        job['ThrottleInterval'] = interval
        hit += 1
        print('throttle', label, interval)
    if hit:
        save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {hit} jobs throttled to {interval}s')


def disable(root):
    """Switch the crash-looping jobs off entirely (idempotent)."""
    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    plist = load(path)
    hit = 0
    for name, job in plist['LaunchDaemons'].items():
        label = job.get('Label', name)
        if not any(sel.lower() in label.lower() for sel in DISABLE):
            continue
        if job.get('Disabled') is True and job.get('RunAtLoad') is False:
            continue
        job['Disabled'] = True
        job['RunAtLoad'] = False
        hit += 1
        print('disable', label)
    if hit:
        save(path, plist, st.st_uid, st.st_gid, st.st_mode & 0o7777)
    print(f'launchd.plist: {hit} jobs disabled')


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    save(os.path.join(d, 'data_ark.plist'), {
        '-ActivationState': 'Activated', '-BrickState': False,
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }, MOBILE, MOBILE, 0o600)
    print('data ark', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], 'System/Library/xpc')):
        sys.exit(__doc__)
    root = sys.argv[1]
    launchd(root)
    # v7 bisekt: v3 (which reached the lock screen) had neither throttle nor
    # disable.  Both are opt-in now so a build can reproduce v3's content on
    # v7's layout: run 42498166705 reached a 124 KB ring and stopped before
    # SpringBoard with mobileactivationd disabled and no seeded data ark.
    if os.environ.get('SWEET_TWEAK_THROTTLE', '0') == '1':
        throttle(root)
    if os.environ.get('SWEET_TWEAK_DISABLE', '0') == '1':
        disable(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
