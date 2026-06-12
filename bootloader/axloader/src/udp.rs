extern crate alloc;

use alloc::{vec, vec::Vec};
use core::{ptr, time::Duration};

use uefi::{
    Handle, Status,
    boot::{self, OpenProtocolAttributes, OpenProtocolParams, ScopedProtocol},
    proto::network::{
        ip4config2::Ip4Config2,
        snp::{ReceiveFlags, SimpleNetwork},
    },
};

use crate::http::KernelLoadError;

const ETHERTYPE_IPV4: u16 = 0x0800;
const IPV4_PROTO_UDP: u8 = 17;
const IPV4_HEADER_LEN: usize = 20;
const UDP_HEADER_LEN: usize = 8;
const ETHERNET_HEADER_LEN: usize = 14;
const CONTROL_MAGIC: &[u8; 8] = b"AXLDUDP1";
const CONTROL_KIND_DATA: u8 = 2;
const CONTROL_HEADER_LEN: usize = 25;
const UDP_CLIENT_PORT: u16 = 40101;
const DATA_IDLE_LIMIT: usize = 10_000;
const POLL_STALL: Duration = Duration::from_millis(1);
const KERNEL_PROGRESS_STEP_PERCENT: usize = 10;
const KERNEL_PROGRESS_BAR_WIDTH: usize = 50;
const MAX_KERNEL_DOWNLOAD_SIZE: usize = 256 * 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UdpDownloadError {
    NetworkUnavailable,
    InvalidConfig,
    PacketTooLarge,
    ReceiveFailed,
    Timeout,
}

pub fn download_sized_body(
    transfer_id: u32,
    server_port: u16,
    block_size: usize,
    expected_size: u64,
) -> Result<Vec<u8>, KernelLoadError> {
    let expected_size = checked_kernel_size(expected_size)?;
    crate::logln!(
        "udp_download_start: size={} server_port={} block_size={}",
        expected_size,
        server_port,
        block_size
    );
    let mut body = vec![0; expected_size];
    download_to_addr(
        transfer_id,
        server_port,
        block_size,
        body.as_mut_ptr(),
        expected_size,
    )
    .map_err(|err| {
        crate::logln!("udp_download_error: {err:?}");
        KernelLoadError::SizeMismatch
    })?;
    Ok(body)
}

fn download_to_addr(
    transfer_id: u32,
    _server_port: u16,
    block_size: usize,
    dst: *mut u8,
    expected_size: usize,
) -> Result<(), UdpDownloadError> {
    if block_size == 0 || block_size > 1400 {
        return Err(UdpDownloadError::InvalidConfig);
    }
    let mut net = SnpUdp::open()?;
    let mut rx = [0u8; 2048];
    net.announce();

    let mut progress = DownloadProgress::new(expected_size);
    progress.print(0);

    let mut received = vec![false; expected_size.div_ceil(block_size)];
    let mut received_bytes = 0usize;
    let mut idle = 0usize;

    while received_bytes < expected_size {
        match net.recv_control(&mut rx, UDP_CLIENT_PORT)? {
            Some(packet)
                if packet.header.kind == CONTROL_KIND_DATA
                    && packet.header.transfer_id == transfer_id =>
            {
                idle = 0;
                let (offset, len) = handle_data_packet(&packet, dst, expected_size, block_size)?;
                let index = offset / block_size;
                if let Some(slot) = received.get_mut(index)
                    && !*slot
                {
                    *slot = true;
                    received_bytes = received_bytes.saturating_add(len);
                    progress.maybe_print(received_bytes.min(expected_size));
                }
            }
            _ => {
                idle += 1;
                if idle >= DATA_IDLE_LIMIT {
                    progress.finish_line();
                    log_missing_blocks(&received, block_size);
                    return Err(UdpDownloadError::Timeout);
                }
                boot::stall(POLL_STALL);
            }
        }
    }

    progress.finish_line();
    Ok(())
}

fn log_missing_blocks(received: &[bool], block_size: usize) {
    let mut missing_count = 0usize;
    let mut first_missing = None;
    let mut last_missing = None;

    for (index, received) in received.iter().copied().enumerate() {
        if !received {
            missing_count += 1;
            first_missing.get_or_insert(index);
            last_missing = Some(index);
        }
    }

    if let Some(first) = first_missing {
        crate::logln!(
            "udp_missing_blocks: count={} first_index={} first_offset={} last_index={}",
            missing_count,
            first,
            first.saturating_mul(block_size),
            last_missing.unwrap_or(first)
        );
    }
}

fn handle_data_packet(
    packet: &ReceivedControlPacket<'_>,
    dst: *mut u8,
    expected_size: usize,
    block_size: usize,
) -> Result<(usize, usize), UdpDownloadError> {
    let offset = packet.header.offset as usize;
    let len = packet.payload.len();
    if offset >= expected_size || offset + len > expected_size || len > block_size {
        return Err(UdpDownloadError::PacketTooLarge);
    }
    unsafe {
        ptr::copy_nonoverlapping(packet.payload.as_ptr(), dst.add(offset), len);
    }
    Ok((offset, len))
}

struct SnpUdp {
    snp: ScopedProtocol<SimpleNetwork>,
    local_ip: [u8; 4],
    local_mac: [u8; 6],
}

impl SnpUdp {
    fn open() -> Result<Self, UdpDownloadError> {
        prepare_network();
        let handles = boot::find_handles::<SimpleNetwork>()
            .map_err(|_| UdpDownloadError::NetworkUnavailable)?;
        for handle in handles.iter().copied() {
            if let Ok(snp) = open_snp(handle) {
                let _ = snp.start();
                let _ = snp.initialize(0, 0);
                let _ = snp.receive_filters(
                    ReceiveFlags::UNICAST | ReceiveFlags::BROADCAST,
                    ReceiveFlags::empty(),
                    false,
                    None,
                );
                let local_mac = <[u8; 6]>::from(snp.mode().current_address);
                if let Some(local_ip) = interface_ip(handle) {
                    crate::logln!(
                        "udp_snp_ready: ip={}.{}.{}.{} \
                         mac={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                        local_ip[0],
                        local_ip[1],
                        local_ip[2],
                        local_ip[3],
                        local_mac[0],
                        local_mac[1],
                        local_mac[2],
                        local_mac[3],
                        local_mac[4],
                        local_mac[5],
                    );
                    return Ok(Self {
                        snp,
                        local_ip,
                        local_mac,
                    });
                }
            }
        }
        Err(UdpDownloadError::NetworkUnavailable)
    }

    fn announce(&self) {
        crate::logln!(
            concat!(
                "AXLOADER NET {{",
                "\"ip\":\"{}.{}.{}.{}\",",
                "\"mac\":\"{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\",",
                "\"udp_port\":{}",
                "}}"
            ),
            self.local_ip[0],
            self.local_ip[1],
            self.local_ip[2],
            self.local_ip[3],
            self.local_mac[0],
            self.local_mac[1],
            self.local_mac[2],
            self.local_mac[3],
            self.local_mac[4],
            self.local_mac[5],
            UDP_CLIENT_PORT,
        );
    }

    fn recv_control<'a>(
        &mut self,
        buffer: &'a mut [u8],
        dst_port: u16,
    ) -> Result<Option<ReceivedControlPacket<'a>>, UdpDownloadError> {
        let len = match self.snp.receive(buffer, None, None, None, None) {
            Ok(len) => len,
            Err(err) if err.status() == Status::NOT_READY => return Ok(None),
            Err(_) => return Err(UdpDownloadError::ReceiveFailed),
        };
        parse_udp_control(&buffer[..len], self.local_ip, dst_port)
    }
}

fn checked_kernel_size(expected_size: u64) -> Result<usize, KernelLoadError> {
    if expected_size == 0 {
        return Err(KernelLoadError::ZeroSize);
    }
    if expected_size > MAX_KERNEL_DOWNLOAD_SIZE as u64 {
        return Err(KernelLoadError::SizeTooLarge);
    }
    Ok(expected_size as usize)
}

fn open_snp(handle: Handle) -> Result<ScopedProtocol<SimpleNetwork>, UdpDownloadError> {
    unsafe {
        boot::open_protocol::<SimpleNetwork>(
            OpenProtocolParams {
                handle,
                agent: boot::image_handle(),
                controller: None,
            },
            OpenProtocolAttributes::GetProtocol,
        )
    }
    .map_err(|_| UdpDownloadError::NetworkUnavailable)
}

fn interface_ip(handle: Handle) -> Option<[u8; 4]> {
    let mut ip4 = unsafe {
        boot::open_protocol::<Ip4Config2>(
            OpenProtocolParams {
                handle,
                agent: boot::image_handle(),
                controller: None,
            },
            OpenProtocolAttributes::GetProtocol,
        )
    }
    .ok()?;
    let info = ip4.get_interface_info().ok()?;
    Some(info.station_addr.octets())
}

fn prepare_network() {
    let Ok(handles) = boot::find_handles::<Ip4Config2>() else {
        return;
    };
    for handle in handles.iter().copied() {
        if let Ok(mut protocol) = unsafe {
            boot::open_protocol::<Ip4Config2>(
                OpenProtocolParams {
                    handle,
                    agent: boot::image_handle(),
                    controller: None,
                },
                OpenProtocolAttributes::GetProtocol,
            )
        } {
            if protocol.ifup().is_ok() {
                return;
            }
        }
    }
}

#[derive(Clone, Copy)]
struct ControlPacket {
    kind: u8,
    transfer_id: u32,
    offset: u32,
}

struct ReceivedControlPacket<'a> {
    header: ControlPacket,
    payload: &'a [u8],
}

fn parse_udp_control<'a>(
    frame: &'a [u8],
    local_ip: [u8; 4],
    dst_port: u16,
) -> Result<Option<ReceivedControlPacket<'a>>, UdpDownloadError> {
    if frame.len() < ETHERNET_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN + CONTROL_HEADER_LEN {
        return Ok(None);
    }
    if read_be16(&frame[12..14]) != ETHERTYPE_IPV4 {
        return Ok(None);
    }
    let ip = ETHERNET_HEADER_LEN;
    if frame[ip] >> 4 != 4 || frame[ip + 9] != IPV4_PROTO_UDP {
        return Ok(None);
    }
    let ihl = usize::from(frame[ip] & 0x0f) * 4;
    if ihl < IPV4_HEADER_LEN || frame.len() < ip + ihl + UDP_HEADER_LEN + CONTROL_HEADER_LEN {
        return Ok(None);
    }
    if frame[ip + 16..ip + 20] != local_ip && frame[ip + 16..ip + 20] != [255, 255, 255, 255] {
        return Ok(None);
    }
    let udp = ip + ihl;
    if read_be16(&frame[udp + 2..udp + 4]) != dst_port {
        return Ok(None);
    }
    let udp_len = usize::from(read_be16(&frame[udp + 4..udp + 6]));
    if udp_len < UDP_HEADER_LEN + CONTROL_HEADER_LEN || udp + udp_len > frame.len() {
        return Ok(None);
    }
    let body = udp + UDP_HEADER_LEN;
    if &frame[body..body + 8] != CONTROL_MAGIC {
        return Ok(None);
    }
    let payload = &frame[body + CONTROL_HEADER_LEN..udp + udp_len];
    Ok(Some(ReceivedControlPacket {
        header: ControlPacket {
            kind: frame[body + 8],
            transfer_id: read_be32(&frame[body + 9..body + 13]),
            offset: read_be32(&frame[body + 13..body + 17]),
        },
        payload,
    }))
}

fn read_be16(bytes: &[u8]) -> u16 {
    u16::from_be_bytes([bytes[0], bytes[1]])
}

fn read_be32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

struct DownloadProgress {
    expected_size: usize,
    next_percent: usize,
}

impl DownloadProgress {
    fn new(expected_size: usize) -> Self {
        Self {
            expected_size,
            next_percent: KERNEL_PROGRESS_STEP_PERCENT,
        }
    }

    fn maybe_print(&mut self, downloaded: usize) {
        let percent = downloaded
            .saturating_mul(100)
            .checked_div(self.expected_size)
            .unwrap_or(0);
        if percent >= self.next_percent || downloaded == self.expected_size {
            self.print(downloaded);
            while self.next_percent <= percent {
                self.next_percent += KERNEL_PROGRESS_STEP_PERCENT;
            }
        }
    }

    fn print(&self, downloaded: usize) {
        let percent = downloaded
            .saturating_mul(100)
            .checked_div(self.expected_size)
            .unwrap_or(0);
        let filled = percent.saturating_mul(KERNEL_PROGRESS_BAR_WIDTH) / 100;
        crate::log!("\rudp_download: [");
        for index in 0..KERNEL_PROGRESS_BAR_WIDTH {
            crate::log!("{}", if index < filled { "#" } else { "-" });
        }
        crate::log!(
            "] {:>3}% {}/{}    ",
            percent,
            downloaded,
            self.expected_size
        );
    }

    fn finish_line(&self) {
        crate::logln!("");
    }
}
