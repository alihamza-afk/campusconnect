# AWS EC2 deployment guide

This document records how the CampusConnect demo was deployed to an AWS EC2 virtual machine and how to maintain it. It describes the current demo architecture; it is not a claim that the setup is a hardened, high-availability production service.

## Architecture

- AWS EC2: Ubuntu Server 24.04 LTS, `t3.micro`, 8 GiB EBS root volume.
- CampusConnect: C++17 server listening on port 8080.
- Process management: systemd starts the application at boot and restarts it if it exits.
- Public access: Cloudflare Quick Tunnel forwards HTTPS traffic to `http://localhost:8080`.
- Database: SQLite stored on the EC2 EBS volume.
- Email: Brevo API through the application's mailer.

The demo was created in the AWS Asia Pacific (Sydney) region. The region can differ if you recreate the instance.

## 1. Create the EC2 instance

Create an Ubuntu Server 24.04 LTS instance. The demo used a `t3.micro` instance and an 8 GiB EBS root volume. Create and download an SSH key pair, and keep the private key on your own computer.

### Security group

Use minimal inbound rules:

| Type | Port | Source |
|---|---:|---|
| SSH | TCP 22 | Your current public IP address only (`/32`) |
| Application | TCP 8080 | Do not add a public inbound rule |

Cloudflare Quick Tunnel makes an outbound connection from the instance, so the application port does not need to be exposed to the internet. Do not allow SSH from `0.0.0.0/0`.

## 2. Connect over SSH

On your computer, restrict access to the downloaded key and connect using the current EC2 public IP:

```bash
chmod 400 /path/to/campusconnect-key.pem
ssh -i /path/to/campusconnect-key.pem ubuntu@YOUR_EC2_PUBLIC_IP
```

Never upload the private key to GitHub or place it in the repository.

## 3. Install dependencies

On the Ubuntu instance:

```bash
sudo apt update
sudo apt upgrade -y
sudo apt install -y build-essential cmake make git g++ pkg-config \
  libsqlite3-dev sqlite3 libssl-dev curl libsodium-dev libcurl4-openssl-dev
```

## 4. Clone, build and test

```bash
git clone https://github.com/alihamza-afk/campusconnect.git
cd campusconnect
make
make test
```

Build and run the tests before deploying changes. The app listens on port 8080 by default.

## 5. Configure server environment

The deployed service reads configuration from `/etc/campusconnect.env`, which is outside the Git repository:

```bash
sudo nano /etc/campusconnect.env
```

Example values (replace placeholders; never commit real values):

```ini
CC_MAIL_MODE=brevo
CC_BREVO_API_KEY=YOUR_BREVO_API_KEY
CC_MAIL_FROM=YOUR_VERIFIED_BREVO_SENDER
CC_MAIL_FROM_NAME=CampusConnect
CC_BASE_URL=https://YOUR-CURRENT-QUICK-TUNNEL.trycloudflare.com
CC_COOKIE_SECURE=1
CC_HSTS=1
CC_TRUST_PROXY=1
```

For local development without real email, use `CC_MAIL_MODE=console` instead.

Restrict the file:

```bash
sudo chown root:root /etc/campusconnect.env
sudo chmod 600 /etc/campusconnect.env
```

### Brevo setup and sender fix

1. Create or use a Brevo account and obtain an API key.
2. Verify the sender email address in Brevo, or authenticate the sending domain.
3. Set `CC_MAIL_MODE=brevo`.
4. Set `CC_BREVO_API_KEY` to the private API key.
5. Set `CC_MAIL_FROM` to the exact verified sender address.
6. Set `CC_BASE_URL` to the public HTTPS URL currently used by testers.

Brevo rejects unverified sender identities. The development fallback `no-reply@campusconnect.local` is not suitable for real delivery. During the live deployment, verification messages initially failed because the sender setting was still using the local development identity. Setting `CC_MAIL_FROM` on EC2 to the verified Brevo sender and restarting the app resolved the issue; the verification email was then received.

Never commit the API key or actual sender address. Keep both only in the server's private environment configuration.

## 6. Run CampusConnect with systemd

The EC2 deployment uses `/etc/systemd/system/campusconnect.service`:

```ini
[Unit]
Description=CampusConnect Web Application
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/home/ubuntu/campusconnect/backend
EnvironmentFile=/etc/campusconnect.env
ExecStart=/home/ubuntu/campusconnect/backend/campusconnect
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now campusconnect
sudo systemctl status campusconnect --no-pager
```

Check the local server:

```bash
curl -I http://127.0.0.1:8080
```

Useful commands:

```bash
sudo systemctl restart campusconnect
sudo journalctl -u campusconnect -n 100 --no-pager
```

Because systemd manages the process, closing the SSH session does not stop the application.

## 7. Publish through Cloudflare Quick Tunnel

Install `cloudflared` using the [official Cloudflare installation instructions](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/).

The tunnel forwards to the local app:

```bash
cloudflared tunnel --url http://localhost:8080
```

Cloudflare prints a public HTTPS address ending in `.trycloudflare.com`. The live demo link is listed in the repository README.

The deployment manages Cloudflare Tunnel with systemd. The service runs the equivalent of:

```bash
cloudflared tunnel --url http://localhost:8080
```

Confirm the installed binary path with `command -v cloudflared` before using it in a systemd unit. The Quick Tunnel URL is temporary and can change when the tunnel is recreated. When it changes, update `CC_BASE_URL` in `/etc/campusconnect.env` and restart the app:

```bash
sudo nano /etc/campusconnect.env
sudo systemctl restart campusconnect
```

This is a demo URL, not a permanent production domain. Anyone who has the public link may be able to reach the app.

## 8. Database persistence and backups

The default database path is:

```
backend/data/campus.db
```

On EC2, the database is stored on the instance's EBS volume and survives application and instance restarts. It can still be lost if the instance or volume is deleted. Back up the database regularly to secure storage outside the instance.

Use SQLite's backup command to create a consistent backup:

```bash
sqlite3 /home/ubuntu/campusconnect/backend/data/campus.db \
  ".backup '/home/ubuntu/campusconnect-backup.db'"
```

Protect backup files because they contain account information.

## 9. Deploy future GitHub updates

After changes have been reviewed and pushed to GitHub:

```bash
cd ~/campusconnect
git pull
make
make test
sudo systemctl restart campusconnect
sudo systemctl status campusconnect --no-pager
```

Do not overwrite the database or delete the data directory when deploying an application update.

## Security checklist

- [ ] SSH is restricted to the administrator's current IP address.
- [ ] Port 8080 is not publicly exposed.
- [ ] `/etc/campusconnect.env` is private and has restrictive permissions.
- [ ] Brevo uses a verified sender address or authenticated domain.
- [ ] API keys, SSH keys, database files and backups are not committed.
- [ ] `CC_BASE_URL` matches the current public HTTPS URL.
- [ ] Secure cookies and HSTS are enabled only when users access the app through HTTPS.
- [ ] SQLite backups are stored outside the EC2 instance.
- [ ] The Quick Tunnel URL is treated as temporary.
