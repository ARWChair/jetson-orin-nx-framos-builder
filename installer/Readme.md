# Jetson Orin NX – FRAMOS Installer

Automated setup for the FRAMOS camera environment and/or VR passthrough on the NVIDIA Jetson Orin NX, powered by Ansible.

## Requirements

- NVIDIA Jetson Orin NX running JetPack (Ubuntu-based)
- Root / `sudo` privileges
- Internet connection

## Installation

### 1. Update the system

```bash
apt-get update && apt-get upgrade -y
```

### 2. Install dependencies

```bash
apt install -y curl ansible
```

### 3. Run the installer

Choose which component(s) to install by setting the `target` variable.

**Installs Framos with VR Passthrough system**

```bash
ansible-playbook Install.yml
```

**FRAMOS IMX environment only**

```bash
ansible-playbook Install.yml -e "target=framos"
```

**VR passthrough only**

```bash
ansible-playbook Install.yml -e "target=vr"
```

> No `target` specified? The playbook defaults to `all`.

## Quick reference

| Target | Command | Installs |
|---|---|---|
| `framos` | `ansible-playbook Install.yml -e "target=framos"` | FRAMOS IMX environment only |
| `vr_passthrough` | `ansible-playbook Install.yml -e "target=vr_passthrough"` | VR passthrough only |
| `all` | `ansible-playbook Install.yml -e "target=all"` | Both components |

## One-liner (full install)

```bash
apt-get update && apt-get upgrade -y && apt install -y curl ansible && ansible-playbook Install.yml -e "target=all"
```

## Troubleshooting

| Issue | Fix |
|---|---|
| `ansible: command not found` | Verify `ansible` was installed (step 2) |
| Playbook fails silently | Re-run with verbose logging: `ansible-playbook Install.yml -vvv` |
| Wrong component installed | Double-check the `target` variable in your command |
