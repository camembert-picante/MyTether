# MyTether

Use your Android phone's internet on your Mac with a USB cable.

## Install

1. Download **MyTether.zip** from the
   [latest release](https://github.com/camembert-picante/MyTether/releases/latest) and open it.
   It runs on Macs with Apple silicon. On an Intel Mac,
   [build it yourself](#build-it-yourself).
2. Drag **MyTether** into your **Applications** folder.
3. Open it. macOS will say it can't check it for malware, because it isn't signed by a paid
   Apple developer account. Click **Done**, then go to **System Settings › Privacy & Security**,
   scroll down, and click **Open Anyway** next to MyTether.
4. Click **Set Up**, and type your Mac password.

You only do this once.

## Connect

1. Plug your phone into your Mac.
2. On the phone, turn on **USB tethering**. It's usually in
   Settings › Network & internet › Hotspot & tethering.

MyTether connects by itself.

## Where to find it

MyTether lives in the menu bar at the top of your screen, as a small plug. Click it to see if
you're connected, check your speed, or disconnect. The gear in that panel has the settings and
Quit.

## If it won't connect

- Check that USB tethering is still on. Some phones turn it off when you unplug.
- Try another cable. Some cables can only charge.
- Unplug the phone and plug it back in.
- Click **Try Again**.

## Remove it

Click the plug, then the gear, then **Remove Background Helper**. Then drag MyTether to the
Trash.

## Good to know

- You need macOS 14 or newer.
- MyTether keeps no log. It only goes online when you click **Check for Updates** in the gear
  menu, to ask GitHub for the newest version.
- Your password is needed because only an administrator can add a network connection on a Mac.
  It installs a small helper that runs only while it's needed and only answers your copy of
  MyTether.
- To find a newer version, click the gear, then **Check for Updates**. After you install it,
  click **Update** and type your password, so the helper knows the new copy.

## Build it yourself

You need Apple's Command Line Tools. In this folder, run:

```bash
make
```

The app appears in the `dist` folder. To run the tests:

```bash
make test
```

## License

MyTether is free and open source under the GNU General Public License, version 3 or any later
version. You can use it, change it and share it, even for money. If you share a changed copy,
you have to share its source code under the same license. The full text is in
[LICENSE](LICENSE).
