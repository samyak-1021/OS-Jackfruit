# OS Jackfruit Problem

## Team

- **Student 1:** Samyak Sanklecha — **PES2UG24CS436**
- **Student 2:** Shriram Chandrasekar — **PES2UG24CS495**

## Project Overview

This repository contains our implementation and experiments for the **OS Jackfruit** project.

At a high level, the project involves building and testing a lightweight container-style runtime (user-space supervisor) along with a kernel-space memory monitor module, and documenting the workflow, outputs, and observations.

## How to Build / Run (Commands used)

> The screenshots below capture the exact workflow we followed while building and running the project.

Common commands used during development and testing:

```bash
# Build (from boilerplate/)
cd boilerplate
make

# CI-safe build
make ci

# (Example) running the engine binary
./engine

# (Example) basic system checks (often used while debugging)
uname -r
lsmod
dmesg | tail -n 50
```

> Note: Some commands in the screenshots may vary slightly depending on which task was being tested.

## Screenshots (with explanation)

All screenshots are available in the [`Screenshots/`](Screenshots) folder.

### 1) ss1.jpeg
![ss1](Screenshots/ss1.jpeg)

**What it shows:** Initial setup / environment preparation step.

**Key commands visible (typical):**
- `sudo apt update`
- `sudo apt install ...`
- `make`

### 2) ss2.jpeg
![ss2](Screenshots/ss2.jpeg)

**What it shows:** Building the project and verifying that compilation succeeds.

**Key commands visible (typical):**
- `cd boilerplate`
- `make`

### 3) ss3.jpeg
![ss3](Screenshots/ss3.jpeg)

**What it shows:** Running the user-space runtime / supervisor and validating CLI behavior.

**Key commands visible (typical):**
- `./engine` (usage output / run invocation)

### 4) ss3(b).jpeg
![ss3b](Screenshots/ss3(b).jpeg)

**What it shows:** Additional run output / an alternate execution path captured separately.

**Key commands visible (typical):**
- `./engine ...` (alternate flags / subcommand)

### 5) ss4.jpeg
![ss4](Screenshots/ss4.jpeg)

**What it shows:** Kernel module / monitor related step (loading, checking logs, or verification).

**Key commands visible (typical):**
- `sudo insmod monitor.ko`
- `lsmod | grep monitor`
- `dmesg | tail`

### 6) ss5.jpeg
![ss5](Screenshots/ss5.jpeg)

**What it shows:** Demonstration of a workload (CPU / I/O / memory) used to test monitoring and scheduling.

**Key commands visible (typical):**
- `./cpu_hog` / `./io_pulse` / `./memory_hog`

### 7) ss6.jpeg
![ss6](Screenshots/ss6.jpeg)

**What it shows:** Observing runtime behavior and logs while workloads run.

**Key commands visible (typical):**
- `dmesg`
- `tail -f <logfile>`

### 8) ss7.jpeg
![ss7](Screenshots/ss7.jpeg)

**What it shows:** Continued testing / multi-step execution output.

**Key commands visible (typical):**
- `make`
- `./engine ...`

### 9) ss8.jpeg
![ss8](Screenshots/ss8.jpeg)

**What it shows:** Additional experiment output (monitor readings / scheduler observations).

**Key commands visible (typical):**
- monitor control commands / experiment script invocation

### 10) ss9.jpeg
![ss9](Screenshots/ss9.jpeg)

**What it shows:** Final verification state (successful run / expected output / cleanup).

**Key commands visible (typical):**
- `sudo rmmod monitor`
- `make clean`

## Notes

- See [`project-guide.md`](project-guide.md) for the full specification of the OS Jackfruit tasks and submission requirements.