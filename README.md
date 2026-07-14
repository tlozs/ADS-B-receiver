# ADS-B Radar Daemon

This project is a high-performance C application designed to receive, process, and decode Mode-S aviation signals, including ADS-B messages, in real-time. It captures raw I/Q samples from Software Defined Radio (SDR) hardware, decodes the telemetry data, and exports aggregated aircraft states to a remote database.

## Core Components

- **[UHD](https://github.com/EttusResearch/uhd):** Used to configure and stream IQ samples from the SDR (USRP B210).
- **[libmodes](https://github.com/watson/libmodes):** Embedded as a static library with slight improvements to process those raw IQ samples and decode Mode S/ADS-B messages.
- **[libcurl](https://github.com/curl/curl):** Handles HTTP POST requests for database ingestion.


## Project Goals

The system was developed with a focus on efficiency, reliability, and low latency:
- **Maximum Performance:** Implemented entirely in C to ensure the highest possible processing speed and strict real-time capabilities.
- **Architectural Control:** By leveraging libmodes strictly for the decoding math, the project maintains complete control over the surrounding system architecture, memory management, and thread synchronization.
- **Reliability:** Designed for continuous, latency-free operation by completely eliminating runtime memory allocations during critical loops and utilizing an asynchronous multithreaded design.

## How It Works

The program operates as a cohesive pipeline divided into three parallel stages:  
- **Reception:** Interfaces with the SDR hardware to stream raw I/Q samples continuously, safely handling potential buffer overruns.  
- **Decoding & Aggregation:** Processes the samples to extract ADS-B messages, applying error correction and physical plausibility filters. It aggregates individual messages into a global, in-memory radar state that tracks the current flight telemetry of each identified aircraft.  
- **Export:** Takes a snapshot of the updated radar state every second and asynchronously pushes the data to a remote server via HTTP POST requests. 

## Installation

Detailed installation instructions can be found in the [INSTALL.md](INSTALL.md) file.