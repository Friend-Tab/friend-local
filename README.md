# friend-local

Your friend.com friend, running on your own computer with your own
Gemini or OpenAI-compatible API key.

There are two parts:

- **libfriend**, the core library: your friend's behaviour and personality,
  which you can build any interface you like on top of.
- **frontends**, a few ready-made ones to get you going: a desktop app, a
  web version in a single `.html` file, and a terminal chat.

## 1. Download your friend file

Do this from friend.com while you still can. Each friend is a separate file,
`friend_<name>.zip`, which holds their memories and your conversation history.

**In the friend.com app:** go back to your list of friends, tap the download
button (the square with an arrow) at the top, then tap the friend you want.
When the row says *"&lt;name&gt; friend file"*, tap **Download**.

**On the website:** open your list of friends, click **Download a friend to a
file** (the arrow icon) at the top, click the friend, then click **Download**
when it's ready.

Keep the zip somewhere private. It contains everything you ever said to them.

## 2. Get an API key

Either one works, but to match existing behaviour most closely use Gemini:

- Gemini: <https://aistudio.google.com/apikey>
- OpenAI: <https://platform.openai.com/api-keys>

## 3. Run it

The downloads below are on the
[releases](https://github.com/Friend-Tab/friend-local/releases) page.

### On your computer (Linux or macOS)

**Linux (x86_64):** download `friend-local-<version>-linux-x86_64.tar.gz`,
unpack it, and run `./friend-local` inside. It needs X11 or Wayland and a
distribution from 2022 or later (Ubuntu 22.04, Debian 12, Fedora 36 or
newer).

**macOS 11 or later:** download `friend-local-<version>-macos.zip` and open
the app. It isn't notarized by Apple, so macOS refuses it the first time;
go to **System Settings › Privacy & Security** and click **Open Anyway**.

Open **Settings** and paste your key, then click **Import a friend file**
and choose your `friend_<name>.zip`, or drop it on the window.

Friends are saved in `~/.local/share/friend-local/friends/` on Linux and
`~/Library/Application Support/friend-local/friends/` on macOS.

To move a friend to another computer, hover over them in the list and click
the download button. That saves them, photos and all, as one
`<name>.friendlocal` file; import it on the other computer the same way as a
friend.com export.

### In a browser

Download `friend-local-<version>.html` and open it. Paste your key under
**Settings**, then **Import** your `friend_<name>.zip`. That works on
Windows too. You can talk to your friend out loud, but the realtime voice
mode is only in the desktop app.

**On a phone,** open <https://friend-tab.github.io/friend-local/> and add it
to your home screen (Share → **Add to Home Screen** on an iPhone, **Install**
on Android). It then opens like an app, works offline, and keeps your friends
on the phone. Phones will not run the downloaded file itself.

Everything stays in that browser on that device, so use the download button
now and then to keep a copy. To build it yourself (you need clang and lld):

```sh
./build.sh web          # build/friend-local.html
```

### Building the desktop app yourself

You need a C compiler and SDL3 (`brew install sdl3`, `pacman -S sdl3`, or
`apt install libsdl3-dev`).

```sh
./build.sh ui
GEMINI_KEY=... ./build/friend_sdl     # or OPENAI_KEY=...
```

Setting the key in the environment is optional; you can type it under
**Settings** instead.

### In a terminal

```sh
./build.sh chat
GEMINI_KEY=... ./build/friendchat friend_<name>.zip
```

It saves the friend as `<name>.friend` beside the zip, with its photographs
in `<name>.blob`; point it at that file next time to carry on. The other
variables the programs here read are listed in `platform/fr_env.h`.

## What's in here

`libfriend/` is the friend itself. It does no I/O; it asks for what it
needs, and the program hosting it does the work. `friend.h` is its entire
interface.
`platform/` is what every host needs around it (TLS, files, the folder of
friends), and `frontend/` holds the desktop window, the web page (the
library compiled to WebAssembly) and the command-line tools. `base/` is the
strings and arenas they share.

## License

MIT. See [LICENSE](LICENSE).
