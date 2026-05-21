# Skylander API LAN discovery

When the Skylander API server is enabled, Cemu is configured for LAN access by default:

- Default HTTP bind host/port: `0.0.0.0:28777`
- Default HTTPS bind host/port: `0.0.0.0:28778` (optional, disabled by default)
- mDNS/DNS-SD service advertised for discovery: `_cemu-skylander._tcp.local`
- UDP fallback discovery listener: port `28779` (probe payload: `CEMU_SKYLANDER_DISCOVERY_V1`)

The API health/info endpoint is available at:

- `GET /api/skylanders/health`
- `GET /api/skylanders/info`

Both return server status, version, endpoint/discovery metadata, and capabilities so clients can validate that they found the correct LAN server.

## Network and firewall requirements

- Phone/tablet and host PC must be on the same local subnet.
- On Windows, use a Private network profile and allow inbound traffic for Cemu.
- Allow inbound TCP on the configured API port (`28777` by default).
- Allow inbound UDP on `5353` (mDNS) and `28779` (fallback discovery) when discovery is needed.

## HTTPS on LAN

HTTPS is optional for local networks. If enabled, mobile clients must trust the configured certificate chain used by Cemu (`HTTPS cert` + `HTTPS key` in Emulated USB Devices settings), otherwise TLS validation will fail.
