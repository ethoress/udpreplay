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
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <pcap/pcap.h>

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
};

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
        ("i,interface", po::value<std::string>(&out.pcap_interface)->default_value(out.pcap_interface), "use libpcap on this interface")
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

class runner
{
private:
    std::chrono::steady_clock::time_point last_stats;
    std::vector<std::uint8_t> buffer;

    std::int64_t packets = 0;
    std::int64_t bytes = 0;
    std::int64_t total_packets = 0;
    std::int64_t total_bytes = 0;
    std::int64_t truncated = 0;
    std::int64_t errors = 0;

protected:
    std::chrono::steady_clock::time_point get_last_stats() const
    {
        return last_stats;
    }

    void update_counters(std::size_t bytes_transferred, bool is_truncated)
    {
        truncated += is_truncated;
        packets++;
        total_packets++;
        bytes += bytes_transferred;
        total_bytes += bytes_transferred;
    }

    void show_stats(std::chrono::steady_clock::time_point now)
    {
        typedef std::chrono::duration<double> duration_t;
        auto elapsed = std::chrono::duration_cast<duration_t>(now - last_stats).count();
        std::cout << total_packets << " (" << packets / elapsed << ") packets\t"
            << total_bytes << " bytes ("
            << bytes * 8.0 / 1e9 / elapsed << " Gb/s)\t"
            << errors << " errors\t" << truncated << " trunc\n";
        packets = 0;
        bytes = 0;
        errors = 0;
        truncated = 0;
        last_stats = now;
    }

    void add_error()
    {
        errors++;
    }
};

class socket_runner : public runner
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
        runner::update_counters(bytes_transferred, bytes_transferred == packet_size);
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
            add_error();
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
                add_error();
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

class pcap_runner : public runner
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
        // TODO: put in the real port number
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
                update_counters(len - (ip_hsize + udp_hsize), truncated);
        }
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

int main(int argc, char **argv)
{
    try
    {
        options opts = parse_args(argc, argv);
        if (opts.pcap_interface == "")
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
