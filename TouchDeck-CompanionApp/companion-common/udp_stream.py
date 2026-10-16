"""Send one encoded packet to the deck and extra panels independently."""

from config_model import device_host
import pc_control


def packet_identity(config):
    payload = {"pc_id": config.get("pc_id", ""), "pc_name": config.get("pc_name", "")}
    if config.get("pc_key") or any(panel.get("key") for panel in config.get("panels", [])):
        payload["cmd_port"] = int(config.get("cmd_port") or pc_control.DEFAULT_CMD_PORT)
        payload["nonces"] = [pc_control.NONCES.issue() for _ in range(4)]
    return payload


def send_packet(sock, config, data, include_deck=True):
    addresses = [config.get("esp32_ip")] if include_deck else []
    addresses.extend(panel["address"] for panel in config.get("panels", []))
    first_error = None
    for address in addresses:
        if not address:
            continue
        try:
            sock.sendto(data, (device_host(address), config.get("udp_port", 4210)))
        except (OSError, ValueError) as exc:
            if first_error is None:
                first_error = exc
    if first_error is not None:
        raise first_error
