"""Decode production packet-builder output independently."""

import os
from pathlib import Path
import subprocess

from Crypto.Cipher import AES
from bleak.backends.device import BLEDevice
from bthome_ble import BTHomeBluetoothDeviceData
from habluetooth import BluetoothServiceInfoBleak
import pytest

ROOT = Path(__file__).resolve().parents[1]
KEY = bytes.fromhex("231d39c1d7cc1ab1aee224cd096db932")
MAC = bytes.fromhex("5448e68f80a5")


@pytest.fixture(scope="session")
def native(tmp_path_factory):
    binary = tmp_path_factory.mktemp("bthome") / "native"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-I", str(ROOT / "include"), str(ROOT / "src/bthome_core.cpp"),
        str(ROOT / "tests/bthome_native.cpp"), "-lcrypto", "-o", str(binary),
    ], check=True)
    return binary


def packet(native, watts, counter=0, energy=()):
    result = subprocess.check_output([
        str(native), str(watts), str(counter), KEY.hex(), MAC.hex(), *map(str, energy)
    ], text=True)
    return bytes.fromhex(result.strip())


def decrypt(data, key=KEY, mac=MAC):
    nonce = mac + data[:3] + data[-8:-4]
    cipher = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=4)
    return cipher.decrypt_and_verify(data[3:-8], data[-4:])


def test_native_invariants(native):
    subprocess.run([str(native)], check=True)


@pytest.mark.parametrize("watts", [0, 1, -1, 1234, -850, 21474836, -21474836])
@pytest.mark.parametrize("counter", [0, 1, 0x11223344, 0xFFFFFFFF])
def test_power_packets(native, watts, counter):
    data = packet(native, watts, counter)
    assert len(data) == 16
    assert data[:3] == bytes.fromhex("d2fc41")
    assert int.from_bytes(data[-8:-4], "little") == counter
    plain = decrypt(data)
    assert plain[0] == 0x5C
    assert int.from_bytes(plain[1:], "little", signed=True) == watts * 100
    assert 3 + 2 + len(data) + 2 + len("OBI-PWR") <= 31


@pytest.mark.parametrize("raw", [0x7FFFFFFF, 0xCCCCCCCD, 21474837, -21474837])
def test_reject_invalid(native, raw):
    result = subprocess.run([str(native), str(raw), "0", KEY.hex(), MAC.hex()],
                            capture_output=True)
    assert result.returncode == 1
    assert not result.stdout


def test_authentication(native):
    data = packet(native, -850, 42)
    for index in range(3, len(data)):
        modified = bytearray(data)
        modified[index] ^= 1
        with pytest.raises(ValueError):
            decrypt(bytes(modified))
    with pytest.raises(ValueError):
        decrypt(data, key=bytes(16))
    with pytest.raises(ValueError):
        decrypt(data, mac=MAC[::-1])


def test_counter_mic_regression(native):
    data = packet(native, -850, 42)
    incorrect = data[:8] + data[12:] + data[8:12]
    with pytest.raises(ValueError):
        decrypt(incorrect)


def service_info(data, timestamp=1000):
    address = ":".join(f"{b:02X}" for b in MAC)
    uuid = "0000fcd2-0000-1000-8000-00805f9b34fb"
    return BluetoothServiceInfoBleak(
        name="OBI-PWR", address=address, rssi=-60, manufacturer_data={},
        service_data={uuid: data[2:]}, service_uuids=[], source="test",
        device=BLEDevice(address, "OBI-PWR", {}), advertisement=None,
        connectable=False, time=timestamp, tx_power=None,
    )


def reading(update, name):
    return next((value.native_value for key, value in update.entity_values.items()
                 if key.key == name), None)


def power_reading(update):
    return reading(update, "power")


@pytest.mark.parametrize("energy", [(0, 0), (4646884, 77402), (0xFFFFFFFF, 0x7FFFFFFE)])
def test_energy_packets(native, energy):
    data = packet(native, -850, 7, energy)
    assert len(data) == 26 and 3 + 2 + len(data) == 31
    plain = decrypt(data)
    assert plain[0] == plain[5] == 0x4D and plain[10] == 0x5C
    assert (int.from_bytes(plain[1:5], "little"), int.from_bytes(plain[6:10], "little")) == energy
    update = BTHomeBluetoothDeviceData(bindkey=KEY).update(service_info(data))
    assert reading(update, "energy_1") == pytest.approx(energy[0] / 1000)
    assert reading(update, "energy_2") == pytest.approx(energy[1] / 1000)
    assert power_reading(update) == -850


@pytest.mark.parametrize("energy", [(0x7FFFFFFF, 1), (1, 0x7FFFFFFF), (0xCCCCCCCD, 1)])
def test_energy_only_as_pair(native, energy):
    data = packet(native, 1234, 7, energy)
    assert len(data) == 16 and decrypt(data)[0] == 0x5C


@pytest.mark.parametrize("watts", [0, 1, -1, 1234, -850])
def test_home_assistant_decoder(native, watts):
    decoder = BTHomeBluetoothDeviceData(bindkey=KEY)
    update = decoder.update(service_info(packet(native, watts, 0)))
    assert power_reading(update) == watts


def test_decoder_counter_progression(native):
    decoder = BTHomeBluetoothDeviceData(bindkey=KEY)
    for sequence, watts in [(42, 1234), (43, -850), (4096, 0)]:
        update = decoder.update(service_info(packet(native, watts, sequence), 1000 + sequence))
        assert power_reading(update) == watts


def test_decoder_wrong_key(native):
    decoder = BTHomeBluetoothDeviceData(bindkey=bytes(16))
    assert power_reading(decoder.update(service_info(packet(native, -850, 42)))) is None


@pytest.fixture(scope="session")
def runtime(tmp_path_factory):
    binary = tmp_path_factory.mktemp("runtime") / "runtime"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fsanitize-undefined-trap-on-error", "-g",
        "-DOBI_BTHOME", "-I", str(ROOT / "tests/fakes"), "-I", str(ROOT / "include"),
        str(ROOT / "src/bthome_core.cpp"), str(ROOT / "src/bthome_ble.cpp"),
        str(ROOT / "tests/bthome_runtime.cpp"), "-lcrypto", "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("scenario", [
    "validation", "disabled_boot", "lifecycle", "storage", "factory_reset", "key_rotation",
    "missing_counter", "advertising_error", "energy_guard",
])
def test_runtime_behaviour(runtime, scenario):
    subprocess.run([str(runtime), scenario], check=True)


def test_runtime_advertisement(runtime):
    advertisement = bytes.fromhex(subprocess.check_output([str(runtime), "packet"], text=True))
    assert len(advertisement) == 31
    fields = {}
    offset = 0
    while offset < len(advertisement):
        size = advertisement[offset]
        fields[advertisement[offset + 1]] = advertisement[offset + 2:offset + 1 + size]
        offset += size + 1
    assert fields[0x01] == b"\x06"
    assert 0x09 not in fields   # name moved to the scan response
    data = fields[0x16]
    assert int.from_bytes(decrypt(data)[11:], "little", signed=True) == -85000
    update = BTHomeBluetoothDeviceData(bindkey=KEY).update(service_info(data))
    assert power_reading(update) == -850
    assert reading(update, "energy_1") == pytest.approx(4646.884)
    assert reading(update, "energy_2") == pytest.approx(77.402)
