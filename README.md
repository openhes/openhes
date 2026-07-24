# openhes

Open source implementation of HES (Home Electronic System) gateway standards ISO/IEC 15045 and 18012.


## Demo device

https://wiki.seeedstudio.com/Dual-Gigabit-Ethernet-Carrier-Board-for-Raspberry-Pi-CM4/



## How to setup SSH

To create an SSH key pair, you can use the ssh-keygen command in your terminal. This built-in utility works seamlessly across macOS, Linux, and modern Windows (via PowerShell or Command Prompt). [1, 2, 3, 4, 5]
## Step 1: Open Your Terminal

* Mac / Linux: Open the default Terminal app.
* Windows 10/11: Open PowerShell or Command Prompt. [4, 5, 6, 7, 8]

## Step 2: Run the Generation Command
Paste the following command to generate a modern, highly secure Ed25519 key pair: [1, 9]

ssh-keygen -t ed25519 -C "your_email@example.com"

(Alternatively, if your destination server requires legacy RSA keys, use: ssh-keygen -t rsa -b 4096 -C "your_email@example.com"). [1, 10]
## Step 3: Respond to the Prompts

   1. Save Location: Press Enter to accept the default hidden directory (~/.ssh/) and default filename (id_ed25519 or id_rsa).
   2. Passphrase: Type a secure passphrase (optional, but highly recommended) and press Enter.
   3. Confirm Passphrase: Re-type the passphrase and press Enter again. [1, 5, 7, 11, 12, 13]

Your screen will display a unique "key fingerprint" and a randomart image, indicating completion. [8, 14, 15, 16, 17]
## Step 4: Locate Your New Keys
Navigate to your SSH directory using your terminal: [14, 18, 19]

cd ~/.ssh
ls

You will find two distinct files: [1, 20]

* id_ed25519: Your private key. Keep this completely safe on your local computer; never share it.
* id_ed25519.pub: Your public key. This is the text you share with remote platforms like [GitHub Accounts](https://docs.github.com/articles/adding-a-new-ssh-key-to-your-github-account) or cloud servers. [5, 7, 12, 21, 22]

------------------------------
## Alternative for Older Windows Versions
If you are on an older Windows version without built-in OpenSSH, download the [PuTTY Utilities Bundle](https://services.wayne.edu/TDClient/277/Portal/KB/Article/20243/How-to-Generate-and-Use-SSH-Keys-with-PuTTY): [4, 23, 24]

   1. Launch PuTTYgen.
   2. Select EdDSA or RSA at the bottom.
   3. Click Generate.
   4. Move your mouse randomly over the blank space to create the encryption data.
   5. Click Save private key and Save public key to local folders. [1, 4, 6, 23, 25, 26]

If you would like to proceed with using your new key, let me know if you need help uploading the public key to a remote server or adding the private key to your local ssh-agent.

[1] [https://www.scaleway.com](https://www.scaleway.com/en/docs/organizations-and-projects/how-to/create-ssh-key/)
[2] [https://docs.alliancecan.ca](https://docs.alliancecan.ca/wiki/Using_SSH_keys_in_Linux)
[3] [https://www.digitalocean.com](https://www.digitalocean.com/community/tutorials/how-to-configure-ssh-key-based-authentication-on-a-linux-server)
[4] [https://docs.cloud.google.com](https://docs.cloud.google.com/compute/docs/connect/create-ssh-keys)
[5] [https://www.youtube.com](https://www.youtube.com/shorts/05gUqcHevzk)
[6] [https://learn.microsoft.com](https://learn.microsoft.com/en-us/viva/glint/setup/sftp-ssh-key-gen)
[7] [https://www.servers.com](https://www.servers.com/kb/linux-administration/how-to-create-a-new-ssh-key-pair)
[8] [https://www.youtube.com](https://www.youtube.com/watch?v=oAbYL3bktMA&t=7)
[9] [https://docs.gitlab.com](https://docs.gitlab.com/user/ssh/)
[10] [https://docs.github.com](https://docs.github.com/en/authentication/connecting-to-github-with-ssh/generating-a-new-ssh-key-and-adding-it-to-the-ssh-agent)
[11] [https://docs.oracle.com](https://docs.oracle.com/en/cloud/cloud-at-customer/occ-get-started/generate-ssh-key-pair.html)
[12] [https://mdl.library.utoronto.ca](https://mdl.library.utoronto.ca/technology/tutorials/generating-ssh-key-pairs-mac)
[13] [https://learn.microsoft.com](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/create-ssh-keys-detailed)
[14] [https://git-scm.com](https://git-scm.com/book/be/v2/Git-on-the-Server-Generating-Your-SSH-Public-Key)
[15] [https://wiki.osuosl.org](https://wiki.osuosl.org/howtos/ssh_key_tutorial.html)
[16] [https://www.purdue.edu](https://www.purdue.edu/science/scienceit/ssh-keys-windows.html)
[17] [https://documentation.sigma2.no](https://documentation.sigma2.no/getting_started/ssh.html)
[18] [https://www.youtube.com](https://www.youtube.com/watch?v=OaKbWJDx3tE&t=3)
[19] [https://www.ibm.com](https://www.ibm.com/docs/en/cognos-analytics/12.0.x?topic=content-generating-ssh-keys-linux-mac)
[20] [https://www.youtube.com](https://www.youtube.com/watch?v=33dEcCKGBO4)
[21] [https://www.youtube.com](https://www.youtube.com/watch?v=Z-HNfaYZ4Dc&t=29)
[22] [https://docs.github.com](https://docs.github.com/articles/adding-a-new-ssh-key-to-your-github-account)
[23] [https://www.purdue.edu](https://www.purdue.edu/science/scienceit/ssh-keys-windows.html)
[24] [https://services.wayne.edu](https://services.wayne.edu/TDClient/277/Portal/KB/Article/20243/How-to-Generate-and-Use-SSH-Keys-with-PuTTY)
[25] [https://www.youtube.com](https://www.youtube.com/watch?v=4DbtuYBLCbk&t=121)
[26] [https://www.ssh.com](https://www.ssh.com/academy/ssh/putty/windows/puttygen)


To upload your public key to a remote server, choose one of the three methods below depending on your operating system and server access. [1, 2]
## Method 1: Using ssh-copy-id (Easiest for Mac and Linux)
This utility automatically logs into your server, locates the correct file, and appends your public key with the proper permissions. [3, 4, 5]

   1. Run the command in your local terminal:

   ssh-copy-id -i ~/.ssh/id_ed25519.pub username@server_ip_address

   2. Enter the password for your remote server when prompted.
   3. Test the connection to log in without a password:

   ssh username@server_ip_address

   [6, 7, 8, 9, 10]

## Method 2: Manually via SSH (Best for Windows PowerShell)
If you do not have ssh-copy-id, you can pipe the key directly through an SSH connection. [11]

   1. Run this single command in your local terminal or PowerShell:

   cat ~/.ssh/id_ed25519.pub | ssh username@server_ip_address "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"

   2. Enter the password for your remote server to complete the transfer. [12, 13, 14, 15, 16]

## Method 3: Paste Manually (Best for Cloud Providers or PuTTY)
If you only have web browser terminal access, or if you used PuTTYgen on Windows, you can paste the text manually. [17]

   1. Copy the key text exactly as it appears.
   * Mac: pbcopy < ~/.ssh/id_ed25519.pub
      * Linux: cat ~/.ssh/id_ed25519.pub
      * Windows (PuTTYgen): Copy the text from the box labeled "Public key for pasting into OpenSSH authorized_keys file". [18, 19, 20, 21, 22]
   2. Log into the server using your current password or cloud console. [23]
   3. Open the authorized_keys file using a text editor like Nano:

   nano ~/.ssh/authorized_keys

   [24]
   4. Paste your public key on a new line at the bottom of the file. [25]
   5. Save and exit (In Nano, press Ctrl+O, Enter, then Ctrl+X). [26, 27]
   6. Set permissions to ensure the server does not reject the key due to loose security:

   chmod 700 ~/.ssh
   chmod 600 ~/.ssh/authorized_keys

   [28, 29, 30]

------------------------------
If you encounter any issues, let me know if you are getting a "Permission denied" error or if you want to learn how to disable password logins completely to make your server fully secure.

[1] [https://dev.to](https://dev.to/altairlage/how-to-create-an-ssh-key-and-configure-key-based-authenication-on-your-linux-server-4fc3)
[2] [https://phoenixnap.com](https://phoenixnap.com/kb/setup-passwordless-ssh)
[3] [https://www.ssh.com](https://www.ssh.com/academy/ssh/copy-id)
[4] [https://oneuptime.com](https://oneuptime.com/blog/post/2026-03-02-copy-ssh-keys-remote-servers-ssh-copy-id-ubuntu/view)
[5] [https://www.digitalocean.com](https://www.digitalocean.com/community/tutorials/how-to-set-up-ssh-keys-on-ubuntu-22-04)
[6] [https://wiki.nushackers.org](https://wiki.nushackers.org/hackerschool/hands-on-self-hosting/server-setup)
[7] [https://security.berkeley.edu](https://security.berkeley.edu/education-awareness/ssh-key-management)
[8] [https://kb.n0c.com](https://kb.n0c.com/en/knowledge-base/how-to-create-an-ssh-key-and-connect-to-an-account/)
[9] [https://try.direct](https://try.direct/blog/how-to-add-public-key-to-your-server)
[10] [https://labex.io](https://labex.io/tutorials/comptia-configure-ssh-public-key-authentication-in-linux-590855)
[11] [https://www.digitalocean.com](https://www.digitalocean.com/community/tutorials/how-to-configure-ssh-key-based-authentication-on-a-linux-server)
[12] [https://www.digitalocean.com](https://www.digitalocean.com/community/tutorials/how-to-set-up-ssh-keys-on-ubuntu-22-04)
[13] [https://www.howtogeek.com](https://www.howtogeek.com/336775/how-to-enable-and-use-windows-10s-built-in-ssh-commands/)
[14] [https://www.hostinger.com](https://www.hostinger.com/in/tutorials/what-is-ssh)
[15] [https://www.atlantic.net](https://www.atlantic.net/vps-hosting/how-to-set-up-ssh-public-and-private-key-in-linux/)
[16] [https://www.virtono.com](https://www.virtono.com/community/tutorial-how-to/how-to-set-up-ssh-keys-on-ubuntu-20-04/)
[17] [https://support.cloudways.com](https://support.cloudways.com/en/articles/5120579-why-should-you-set-up-ssh-keys)
[18] [https://learn.microsoft.com](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/create-ssh-keys-detailed)
[19] [https://medium.com](https://medium.com/@jakewies/accessing-remote-machines-using-ssh-55a0fdf5e9d8)
[20] [https://kb.fastpanel.direct](https://kb.fastpanel.direct/quick_start/how-to-set-up-ssh-keys/)
[21] [https://www.digitalocean.com](https://www.digitalocean.com/community/tutorials/how-to-set-up-ssh-keys-on-ubuntu-22-04)
[22] [https://www.mediawiki.org](https://www.mediawiki.org/wiki/SSH_keys/es)
[23] [https://upcloud.com](https://upcloud.com/docs/guides/connecting-to-your-server/)
[24] [https://docs.hostdime.com](https://docs.hostdime.com/linux-server/The-Guide-to-Generating-and-Uploading-SSH-Keys_9109515/)
[25] [https://docs.hostdime.com](https://docs.hostdime.com/linux-server/The-Guide-to-Generating-and-Uploading-SSH-Keys_9109515/)
[26] [https://www.linkedin.com](https://www.linkedin.com/pulse/ssh-key-authentication-connection-from-client-server-david-zhu-hjd0c)
[27] [https://wiki.csclub.uwaterloo.ca](https://wiki.csclub.uwaterloo.ca/How_to_SSH)
[28] [https://www.amaze.au](https://www.amaze.au/resources/articles/how-to-connect-to-the-server-using-ssh-keys-on-windows-10-using-putty/)
[29] [https://linuxize.com](https://linuxize.com/post/how-to-setup-passwordless-ssh-login/)
[30] [https://replay.software](https://replay.software/help/echo/ssh-keys)





SSH strictly requires that your private key files are hidden from other users on your system. A permission setting of 0644 means anyone can read your file, so SSH blocks it for your security.You can fix this instantly by changing the file permissions so only your user account can read or write to it.

```
chmod 600 /home/tomghuang/.ssh/id_ed25519-raspberrypi
```


# How to setup WSL2 so this environment can see Raspberry Pi CM4

To fix this, you do not need a complex bridge. WSL2 already uses Windows as a network bridge by default, but it operates on a separate virtual network, which can block direct connections to your local Wi-Fi router.
Here are the two easiest ways to solve this. Option 1 changes WSL2 to sit directly on your Wi-Fi network, while Option 2 uses a quick copy-paste trick using Windows folders.
------------------------------
## Option 1: Put WSL2 directly on your Wi-Fi network (Recommended)
You can configure WSL2 to use Mirrored Networking Mode. This forces WSL2 to share the exact same IP address and network interface as your Windows host, allowing it to see the Raspberry Pi instantly. [1]

   1. Open Windows Notepad.
   2. Paste the following text into the blank file:

   [wsl2]
   networkingMode=mirrored

   3. Save the file in your Windows user profile directory (C:\Users\<YourUsername>\) and name it exactly .wslconfig (Make sure it does not end in .txt).
   4. Restart WSL2 by opening Windows PowerShell and running:

   wsl --shutdown

   5. Open WSL2 again. Your WSL2 terminal can now directly ping and scp to your Raspberry Pi's local IP address. [2]

------------------------------
## Option 2: Use Windows as a file bridge (No config needed)
If you prefer not to change network settings, you can use the Windows file system as a middleman. WSL2 can natively access your Windows hard drive.

   1. Compile your file inside WSL2.
   2. Copy the binary to your Windows Desktop from inside your WSL2 terminal:

   cp hello_pi /mnt/c/Users/<YourWindowsUsername>/Desktop/

   3. Send it to the Pi by opening a standard Windows Command Prompt (cmd) or PowerShell window and running your scp command from there:

   cd ~\Desktop
   scp hello_pi pi@<your_pi_ip>:/home/pi/

   [3]

If you try Option 1 and it still cannot find the Pi, let me know if you are using a third-party firewall/antivirus or a VPN on Windows, as these often block WSL2 traffic.

[1] [https://blog.devgenius.io](https://blog.devgenius.io/transferring-files-between-two-wsl2-instances-using-rsync-4931f701e8a2)
[2] [https://stackoverflow.com](https://stackoverflow.com/questions/64763147/access-a-localhost-running-in-windows-from-inside-wsl-2)
[3] [https://www.hanselman.com](https://www.hanselman.com/blog/how-to-ssh-into-wsl2-on-windows-10-from-an-external-machine)


---

Measure CPU temperature:

```
vcgencmd measure_temp
```

A Raspberry Pi CPU will begin to thermal throttle when it exceeds 80°C.

---

Optimized tool selectionYou do not need to modify sources.list at all — just enable multi-arch and add the ARM architectures to `dpkg`. The same repository serves packages for all architectures automatically.

### Step-by-step

1. **Add ARM architectures to dpkg**
   ```bash
   sudo dpkg --add-architecture armhf    # arm32 (Raspberry Pi 32-bit)
   sudo dpkg --add-architecture arm64    # arm64 (Raspberry Pi 64-bit)
   ```

2. **Update package index**
   ```bash
   sudo apt update
   ```

3. **Install cross-compilation toolchain**
   ```bash
   sudo apt install -y gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf   # arm32
   sudo apt install -y gcc-aarch64-linux-gnu   g++-aarch64-linux-gnu     # arm64
   ```

4. **Install ARM libraries** (hiredis, libxml2, etc. needed by your project)
   ```bash
   sudo apt install -y libhiredis-dev:armhf  libxml2-dev:armhf
   sudo apt install -y libhiredis-dev:arm64  libxml2-dev:arm64
   ```

5. **Verify**
   ```bash
   arm-linux-gnueabihf-gcc --version   # arm32
   aarch64-linux-gnu-gcc --version     # arm64
   ```

### When `sources.list` *does* need changing

If your sources.list uses `deb [arch=amd64] ...` (restricted to amd64 only), change each line to remove the `[arch=amd64]` restriction or extend it:

```
# Before
deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble main

# After
deb http://archive.ubuntu.com/ubuntu noble main
```

That's it — the same apt cache then serves packages for all enabled architectures.

### Using in your CMakeLists.txt

Once the ARM toolchains are installed, write a CMake toolchain file for the Pi. I can create `cmake/arm64-toolchain.cmake` and `cmake/armhf-toolchain.cmake` in your project and show you how to build the `redis_pub`/`redis_sub` targets for ARM with:
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm64-toolchain.cmake ..
cmake --build . --target redis_pub redis_sub
```

Would you like me to generate those toolchain files and wire them into your CMakeLists.txt?


---

# How to setup a cross-compilation environment for Raspberry Pi CM4 on WSL2

Your WSL2 is an amd64 environment, so by default apt only knows about amd64 packages. To cross-compile for Raspberry Pi (arm64/aarch64), you need to add the arm64 architecture to dpkg and configure apt sources.

## 1. Add the arm64 architecture

```bash
sudo dpkg --add-architecture arm64
```

## 2. Update package lists

```bash
sudo apt update
```

## 3. Install arm64 cross-compilation toolchain (if not already done)

```bash
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

## 4. Install arm64 libraries needed by your project

```bash
sudo apt install libglib2.0-dev:arm64
sudo apt install libmount-dev:arm64
sudo apt install libselinux1-dev:arm64
```

The `:arm64` suffix tells apt to install the arm64 variant.

If you see "Package 'libhiredis-dev' has no installation candidate":

### 4.1 See what source entries you currently have

```bash
cat /etc/apt/sources.list
ls /etc/apt/sources.list.d/
```

### 4.2 Add ports.ubuntu.com entries for arm64

`archive.ubuntu.com` only hosts amd64 (and i386) packages — arm64/armhf packages live at `ports.ubuntu.com`. So, if you see errors like "Package 'libhiredis-dev' has no installation candidate", you may need to add the `ports.ubuntu.com` repository to your `/etc/apt/sources.list`:

```
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse
```

### 4.3 Restrict existing repos to `amd64`

Modify your existing sources to add [arch=amd64] so apt won't try to find arm64 packages there. For example:

```
deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted
```