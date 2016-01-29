/* Copyright 2015 SKA South Africa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <memory>
#include <vector>
#include <list>
#include <chrono>
#include <cstring>
#include <sstream>
#include <cerrno>
#include <atomic>
#include <thread>
#include <future>
#include <system_error>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <pcap/pcap.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>

namespace asio = boost::asio;
namespace po = boost::program_options;
using boost::asio::ip::udp;

struct options
{
    std::string host = "";
    std::string port = "8888";
    std::size_t socket_size = 0;
    std::size_t packet_size = 16384;
    std::size_t buffer_size = 0;
    std::string pcap_interface = "";
    int poll = 0;
    bool pfpacket = false;
};

[[noreturn]] static void throw_errno()
{
    throw std::system_error(errno, std::system_category());
}

static options parse_args(int argc, char **argv)
{
    options out;

    po::options_description desc;
    desc.add_options()
        ("host", po::value<std::string>(&out.host)->default_value(out.host), "destination host")
        ("port", po::value<std::string>(&out.port)->default_value(out.port), "destination port")
        ("socket-size", po::value<std::size_t>(&out.socket_size)->default_value(out.socket_size), "receive buffer size (0 for system default)")
        ("packet-size", po::value<std::size_t>(&out.packet_size)->default_value(out.packet_size), "maximum packet size")
        ("buffer-size", po::value<std::size_t>(&out.buffer_size)->default_value(out.buffer_size), "size of receive arena (0 for packet size)")
        ("poll", po::value<int>(&out.poll)->default_value(out.poll), "make up to this many synchronous reads")
        ("interface,i", po::value<std::string>(&out.pcap_interface)->default_value(out.pcap_interface), "use libpcap on this interface")
        ("pfpacket", po::bool_switch(&out.pfpacket)->default_value(out.pfpacket), "use low-level PF_PACKET instead of pcap")
        ;
    try
    {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv)
                  .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                  .options(desc)
                  .run(), vm);
        po::notify(vm);
        return out;
    }
    catch (po::error &e)
    {
        std::cerr << e.what() << "\n\n";
        std::cerr << "Usage: udpcount [options]\n";
        std::cerr << desc;
        throw;
    }
}

template<typename T>
class metrics
{
public:
    T packets;
    T bytes;
    T total_packets;
    T total_bytes;
    T truncated;
    T errors;

    metrics()
    {
        packets = 0;
        bytes = 0;
        total_packets = 0;
        total_bytes = 0;
        truncated = 0;
        errors = 0;
    }

    void add_packet(std::size_t bytes_transferred, bool is_truncated)
    {
        truncated += is_truncated;
        packets++;
        total_packets++;
        bytes += bytes_transferred;
        total_bytes += bytes_transferred;
    }

    void add_error()
    {
        errors++;
    }

    void reset()
    {
        packets = 0;
        bytes = 0;
        errors = 0;
        truncated = 0;
    }

    void show_stats(double elapsed)
    {
        std::cout << total_packets << " (" << packets / elapsed << ") packets\t"
            << total_bytes << " bytes ("
            << bytes * 8.0 / 1e9 / elapsed << " Gb/s)\t"
            << errors << " errors\t" << truncated << " trunc\n";
    }

    template<typename U>
    metrics &operator+=(const metrics<U> &other)
    {
        packets += other.packets;
        bytes += other.bytes;
        total_packets += other.total_packets;
        total_bytes += other.total_bytes;
        truncated += other.truncated;
        errors += other.errors;
        return *this;
    }
};

template<typename T>
class runner
{
private:
    std::chrono::steady_clock::time_point last_stats;
    std::vector<std::uint8_t> buffer;

protected:
    metrics<T> counters;

    std::chrono::steady_clock::time_point get_last_stats() const
    {
        return last_stats;
    }

    void show_stats(std::chrono::steady_clock::time_point now)
    {
        typedef std::chrono::duration<double> duration_t;
        auto elapsed = std::chrono::duration_cast<duration_t>(now - last_stats).count();
        counters.show_stats(elapsed);
        counters.reset();
        last_stats = now;
    }
};

class socket_runner : public runner<std::int64_t>
{
private:
    asio::io_service io_service;
    udp::socket socket;
    asio::basic_waitable_timer<std::chrono::steady_clock> timer;
    std::vector<std::uint8_t> buffer;
    const std::size_t packet_size;
    const int poll;
    std::size_t offset = 0;
    udp::endpoint remote;

    void enqueue_receive()
    {
        using namespace std::placeholders;
        socket.async_receive_from(
            asio::buffer(buffer.data() + offset, packet_size),
            remote,
            std::bind(&socket_runner::packet_handler, this, _1, _2));
    }

    void enqueue_wait()
    {
        using namespace std::placeholders;
        timer.async_wait(std::bind(&socket_runner::timer_handler, this, _1));
    }

    void update_counters(std::size_t bytes_transferred)
    {
        counters.add_packet(bytes_transferred, bytes_transferred == packet_size);
        offset += bytes_transferred;
        // Round up to a cache line offset
        offset = ((offset + 63) & ~63);
        if (offset >= buffer.size() - packet_size)
            offset = 0;
    }

    void packet_handler(const boost::system::error_code &error,
                        std::size_t bytes_transferred)
    {
        if (error)
            counters.add_error();
        else
            update_counters(bytes_transferred);
        for (int i = 0; i < poll; i++)
        {
            boost::system::error_code ec;
            bytes_transferred = socket.receive_from(
                asio::buffer(buffer.data() + offset, packet_size),
                remote, 0, ec);
            if (ec == asio::error::would_block)
                break;
            else if (ec)
                counters.add_error();
            else
                update_counters(bytes_transferred);
        }
        enqueue_receive();
    }

    void timer_handler(const boost::system::error_code &error)
    {
        auto now = timer.expires_at();
        show_stats(now);
        timer.expires_at(timer.expires_at() + std::chrono::seconds(1));
        enqueue_wait();
    }

public:
    explicit socket_runner(const options &opts)
        : socket(io_service), timer(io_service),
        buffer(std::max(opts.packet_size, opts.buffer_size)), packet_size(opts.packet_size), poll(opts.poll)
    {
        udp::resolver resolver(io_service);
        udp::resolver::query query(
            udp::v4(), opts.host, opts.port,
            udp::resolver::query::passive | udp::resolver::query::address_configured);
        auto endpoint = *resolver.resolve(query);
        socket.open(udp::v4());
        socket.bind(endpoint);
        socket.non_blocking(true);

        if (opts.socket_size != 0)
        {
            socket.set_option(udp::socket::receive_buffer_size(opts.socket_size));
            udp::socket::receive_buffer_size actual;
            socket.get_option(actual);
            if ((std::size_t) actual.value() != opts.socket_size)
            {
                std::cerr << "Warning: requested socket buffer size of " << opts.socket_size
                    << " but actual size is " << actual.value() << '\n';
            }
        }
        timer.expires_from_now(std::chrono::seconds(1));

        enqueue_wait();
        enqueue_receive();
    }

    void run()
    {
        io_service.run();
    }
};

class pcap_runner : public runner<std::int64_t>
{
private:
    pcap_t *cap;

    pcap_runner(const pcap_runner &) = delete;
    pcap_runner &operator=(const pcap_runner &) = delete;

    static void check_status(int status)
    {
        if (status != 0)
            throw std::runtime_error(pcap_statustostr(status));
    }

    void process_packet(const struct pcap_pkthdr *h, const u_char *bytes)
    {
        const unsigned int eth_hsize = 14;
        bpf_u_int32 len = h->caplen;
        bool truncated = h->len != len;
        if (len > eth_hsize)
        {
            bytes += eth_hsize;
            len -= eth_hsize;
            const unsigned int ip_hsize = (bytes[0] & 0xf) * 4;
            const unsigned int udp_hsize = 8;
            if (len >= ip_hsize + udp_hsize)
                counters.add_packet(len - (ip_hsize + udp_hsize), truncated);
        }
    }

public:
    explicit pcap_runner(const options &opts)
    {
        char errbuf[PCAP_ERRBUF_SIZE];
        cap = pcap_create(opts.pcap_interface.c_str(), errbuf);
        if (cap == NULL)
            throw std::runtime_error(std::string(errbuf));
        check_status(pcap_set_snaplen(cap, opts.packet_size));
        if (opts.socket_size != 0)
            check_status(pcap_set_buffer_size(cap, opts.socket_size));
        check_status(pcap_set_timeout(cap, 10));
        check_status(pcap_activate(cap));
        int ret = pcap_set_datalink(cap, DLT_EN10MB);
        if (ret != 0)
            throw std::runtime_error(std::string(pcap_geterr(cap)));
        ret = pcap_setdirection(cap, PCAP_D_IN);
        if (ret != 0)
            throw std::runtime_error(std::string(pcap_geterr(cap)));

        struct bpf_program fp;
        std::ostringstream program;
        program << "udp dst port " << opts.port;
        if (opts.host != "")
            program << " dst " << opts.host;
        if (pcap_compile(cap, &fp, program.str().c_str(), 1, PCAP_NETMASK_UNKNOWN) == -1)
            throw std::runtime_error("Failed to parse filter");
        if (pcap_setfilter(cap, &fp) == -1)
        {
            pcap_freecode(&fp);
            throw std::runtime_error(std::string(pcap_geterr(cap)));
        }
        pcap_freecode(&fp);
    }

    ~pcap_runner()
    {
        pcap_close(cap);
    }

    void run()
    {
        while (true)
        {
            struct pcap_pkthdr *pkt_header;
            const u_char *pkt_data;
            int status = pcap_next_ex(cap, &pkt_header, &pkt_data);
            switch (status)
            {
            case 1:
                // Valid packet
                process_packet(pkt_header, pkt_data);
                break;
            case 0:
                // Timeout expired; this is harmless
                break;
            case -1:
                // Error
                throw std::runtime_error(std::string(pcap_geterr(cap)));
            default:
                throw std::runtime_error("unexpected return from pcap_next_ex");
            }
            auto now = std::chrono::steady_clock::now();
            if (now - get_last_stats() >= std::chrono::seconds(1))
                show_stats(now);
        }
    }
};

template<typename T>
static void apply_offset(T *&out, void *in, std::ptrdiff_t offset)
{
    out = reinterpret_cast<T *>(reinterpret_cast<std::uint8_t *>(in) + offset);
}

template<typename T>
static void apply_offset(const T *&out, const void *in, std::ptrdiff_t offset)
{
    out = reinterpret_cast<const T *>(reinterpret_cast<const std::uint8_t *>(in) + offset);
}

class file_descriptor : public boost::noncopyable
{
public:
    int fd;

    explicit file_descriptor(int fd = -1) : fd(fd) {}

    file_descriptor(file_descriptor &&other) : fd(other.fd)
    {
        other.fd = -1;
    }

    ~file_descriptor()
    {
        if (fd != -1)
            close(fd);
    }

};

class memory_map : public boost::noncopyable
{
public:
    std::uint8_t *ptr;
    std::size_t length;

    memory_map() : ptr(NULL), length(0) {}
    memory_map(std::uint8_t *ptr, std::size_t length) : ptr(ptr), length(length) {}

    memory_map(memory_map &&other) : ptr(other.ptr), length(other.length)
    {
        other.ptr = NULL;
        other.length = 0;
    }

    ~memory_map()
    {
        if (ptr != NULL)
            munmap(ptr, length);
    }
};

class pfpacket_runner : public runner<std::atomic<std::int64_t>>
{
private:
    struct thread_data_t
    {
        file_descriptor fd;
        memory_map map;
    };

    tpacket_req3 ring_req;
    std::vector<thread_data_t> thread_data;

    void prepare_thread_data(thread_data_t &data, const options &opts)
    {
        int status;
        // Create the socket
        int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0)
            throw_errno();
        data.fd.fd = fd;
        // Bind it to interface
        if (opts.pcap_interface != "")
        {
            ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, opts.pcap_interface.c_str(), sizeof(ifr.ifr_name));
            status = ioctl(fd, SIOCGIFINDEX, &ifr);
            if (status < 0)
                throw_errno();
            sockaddr_ll addr;
            memset(&addr, 0, sizeof(addr));
            addr.sll_family = AF_PACKET;
            addr.sll_protocol = htons(ETH_P_ALL);
            addr.sll_ifindex = ifr.ifr_ifindex;
            status = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
            if (status < 0)
                throw_errno();
        }
        // Join the FANOUT group
        int fanout = (getpid() & 0xffff) | (PACKET_FANOUT_CPU << 16);
        status = setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &fanout, sizeof(fanout));
        if (status < 0)
            throw_errno();
        // Set to version 3
        int version = TPACKET_V3;
        status = setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version));
        if (status < 0)
            throw_errno();
        // Set up the ring buffer
        status = setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &ring_req, sizeof(ring_req));
        if (status < 0)
            throw_errno();
        std::size_t length = ring_req.tp_block_size * ring_req.tp_block_nr;
        void *ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
        if (ptr == NULL)
            throw_errno();
        data.map.ptr = (std::uint8_t *) ptr;
        data.map.length = length;
    }

    void process_packet(const tpacket3_hdr *header, metrics<std::int64_t> &local_counters)
    {
        bool truncated = header->tp_snaplen != header->tp_len;
        const ethhdr *eth;
        const iphdr *ip;
        apply_offset(eth, header, header->tp_mac);
        apply_offset(ip, eth, ETH_HLEN);
        if (eth->h_proto == htons(ETH_P_IP))
        {
            // TODO: check for IP options, and IPv6
            local_counters.add_packet(header->tp_len - ETH_HLEN - sizeof(iphdr) - sizeof(udphdr), truncated);
        }
    }

public:
    explicit pfpacket_runner(const options &opts)
    {
        // Set up ring buffer parameters
        memset(&ring_req, 0, sizeof(ring_req));
        ring_req.tp_block_size = 1 << 22;
        ring_req.tp_frame_size = 1 << 11;
        ring_req.tp_block_nr = 1 << 6;
        ring_req.tp_frame_nr = ring_req.tp_block_size / ring_req.tp_frame_size * ring_req.tp_block_nr;
        ring_req.tp_retire_blk_tov = 10;

        // Create per-thread sockets
        int threads = std::thread::hardware_concurrency();
        thread_data.resize(threads);
        for (int i = 0; i < threads; i++)
            prepare_thread_data(thread_data[i], opts);
    }

    void run_thread(thread_data_t &data, int cpu)
    {
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        CPU_SET(cpu, &affinity);
        int status = sched_setaffinity(0, sizeof(affinity), &affinity);
        if (status < 0)
            throw_errno();

        unsigned int next_block = 0;
        pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = data.fd.fd;
        pfd.events = POLLIN | POLLERR;
        while (true)
        {
            tpacket_block_desc *block_desc;
            apply_offset(block_desc, data.map.ptr, next_block * ring_req.tp_block_size);
            std::atomic_thread_fence(std::memory_order_acquire);
            while (!(block_desc->hdr.bh1.block_status & TP_STATUS_USER))
            {
                int status = poll(&pfd, 1, -1);
                if (status < 0)
                    throw_errno();
                std::atomic_thread_fence(std::memory_order_acquire);
            }

            std::size_t num_packets = block_desc->hdr.bh1.num_pkts;
            tpacket3_hdr *header;
            apply_offset(header, block_desc, block_desc->hdr.bh1.offset_to_first_pkt);
            metrics<std::int64_t> local_counters;
            for (std::size_t i = 0; i < num_packets; i++)
            {
                process_packet(header, local_counters);
                apply_offset(header, header, header->tp_next_offset);
            }
            counters += local_counters;

            block_desc->hdr.bh1.block_status = TP_STATUS_KERNEL;
            std::atomic_thread_fence(std::memory_order_release);
            next_block++;
            if (next_block == ring_req.tp_block_nr)
                next_block = 0;
        }
    }

    void run()
    {
        std::vector<std::future<void>> futures;
        int cpu = 0;
        for (auto &data : thread_data)
        {
            auto call = [&data, cpu, this] { run_thread(data, cpu); };
            futures.push_back(std::async(std::launch::async, call));
            cpu++;
        }
        auto now = std::chrono::steady_clock::now();
        while (true)
        {
            now += std::chrono::seconds(1);
            std::this_thread::sleep_until(now);
            show_stats(now);
        }
    }
};

int main(int argc, char **argv)
{
    try
    {
        options opts = parse_args(argc, argv);
        if (opts.pfpacket)
        {
            pfpacket_runner r(opts);
            r.run();
        }
        else if (opts.pcap_interface == "")
        {
            socket_runner r(opts);
            r.run();
        }
        else
        {
            pcap_runner r(opts);
            r.run();
        }
    }
    catch (po::error &e)
    {
        return 1;
    }
    catch (std::runtime_error &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}