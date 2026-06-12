# ADS-B-receiver

This project aims to build a small C-based ADS-B receiver pipeline using:

- [UHD](https://github.com/EttusResearch/uhd) to configure and stream IQ samples from the SDR (USRP B210),
- [libmodes](https://github.com/watson/libmodes) to process those raw IQ samples and decode ADS-B messages.

The expected output is a C struct containing the decoded aircraft information (for example identifier, position, altitude, and velocity fields when available).
