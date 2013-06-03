#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <iostream>
#include <string>
#include <set>
#include <queue>
#include <vector>

#define BOOST_SPIRIT_USE_PHOENIX_V3
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/fusion/adapted/struct/define_struct.hpp>
#include <boost/format.hpp>

#define BUFFER_SIZE 4096
#define NAMESTYLE "rekri%03d"
#define PORT 6659
#define TIMEOUT (3 * 60)

using namespace boost::fusion::operators;

BOOST_FUSION_DEFINE_STRUCT(, irc_server,
    (std::string, host)
    (int, port)
);

BOOST_FUSION_DEFINE_STRUCT(, irc_server_channel,
    (irc_server, server)
    (std::string, channel)
);

BOOST_FUSION_DEFINE_STRUCT(, message,
    (std::vector<irc_server_channel>, channel)
    (std::string, content)
);

int randn(int n)
{
  return (((double) rand() / (RAND_MAX)) + 1) * n - n;
}

struct irc_connection
{
  irc_connection(irc_server const& server_)
    : server(server_), fd(-1), swap_buffer_size(0), swap_dropped(false), last_activity(0)
  {
  }

  void connect()
  {
    if( fd != -1 && (time(0) - last_activity) >= TIMEOUT )
    {
      std::cout << "connection timed out" << std::endl;
      close();
    }

    if(fd != -1)
      return;

    std::cout << "resolving " << server.host << std::endl;
    struct hostent* host = gethostbyname(server.host.c_str());
    if(!host)
    {
      perror("gethostbyname");
      return;
    }

    fd = socket(PF_INET, SOCK_STREAM, 0);
    if(fd < 0)
    {
      perror("TCP socket");
      return;
    }
    int flags;
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      perror("fcntl O_NONBLOCK");
      close();
      return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server.port);
    memcpy(&addr.sin_addr, host->h_addr, 4);
    std::cout << "connect to " << inet_ntoa(addr.sin_addr) << " on port " << server.port << std::endl;
    if(::connect(fd, (sockaddr const*)&addr, sizeof addr) < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
      perror("connect");
      close();
      return;
    }
    last_activity = time(0);

    nick = boost::str(boost::format(NAMESTYLE) % randn(1000));
    for(std::set<std::string>::iterator it = joined.begin(); it != joined.end(); ++it)
      write_queue.push_front("JOIN " + *it);
    write_queue.push_front("USER rekri 0 * :rekri relaying client");
    write_queue.push_front("NICK " + nick);
  }

  void close()
  {
    if(fd != 1)
    {
      ::close(fd);
      fd = -1;
    }
  }

  void join(std::string const& channel)
  {
    connect();
    if(joined.insert(channel).second)
      enqueue("JOIN " + channel);
  }

  void enqueue(std::string const& s)
  {
    connect();
    std::cout << "enqueue " << s << std::endl;
    write_queue.push_back(s);
  }

  void privmsg(std::string const& channel, std::string const& msg)
  {
    join(channel);
    enqueue("PRIVMSG " + channel + " :" + msg);
  }

  void write()
  {
    if(write_queue.empty())
      return;

    std::string& s = write_queue.front();
    std::cout << "write " << s << std::endl;
    ssize_t n = ::write(fd, (s + "\r\n").c_str(), s.size() + 2);
    if(n < 0 || (size_t)n != s.size() + 2)
    {
      perror("write");
      close();
      connect();
      return;
    }
    last_activity = time(0);
    write_queue.pop_front();
  }

  // split packets into lines, lines longer than BUFFER_SIZE dropped
  void read()
  {
    char buffer[BUFFER_SIZE];
    memcpy(buffer, swap_buffer, swap_buffer_size);

    ssize_t n = ::read(fd, buffer + swap_buffer_size, sizeof buffer - swap_buffer_size);
    if(n < 0)
    {
      perror("read");
      close();
      return;
    }
    if(n == 0)
    {
      std::cout << "connection reset by peer" << std::endl;
      close();
      return;
    }
    last_activity = time(0);

    size_t len = swap_buffer_size + (size_t)n;
    size_t i = 0;
    size_t j = 0;
    for(; i != len; ++i)
    {
      if(buffer[i] == '\r')
      {
        buffer[i] = '\0';
      }

      if(buffer[i] == '\n')
      {
        if(!swap_dropped)
          read_message(buffer+j);
        else
          swap_dropped = false;
        j = i+1;
      }
    }

    memcpy(swap_buffer, buffer+j, len-j);
    swap_buffer_size = len-j;
    if(swap_buffer_size == sizeof buffer)
    {
      swap_buffer_size = 0;
      swap_dropped = true;
    }
  }

  void read_message(const char* msg)
  {
    std::cout << "received " << msg << std::endl;

    if(!strncmp(msg, "PING", 4) && (msg[4] == '\0' || msg[4] == ' '))
    {
      enqueue(std::string("PONG") + (msg+4));
      return;
    }
  }

  irc_server server;
  std::string nick;
  int fd;
  std::set<std::string> joined;
  char swap_buffer[BUFFER_SIZE];
  size_t swap_buffer_size;
  bool swap_dropped;
  time_t last_activity;

  std::deque<std::string> write_queue;
};

std::vector<irc_connection> connected_servers;

irc_connection& connection_by_server(irc_server const& server)
{
  irc_connection* connection = 0;

  for(size_t i=0; i!=connected_servers.size(); ++i)
    if(connected_servers[i].server == server)
      connection = &connected_servers[i];

  if(!connection)
  {
    connected_servers.push_back(irc_connection(server));
    connection = &connected_servers.back();
  }

  return *connection;
}

// pseudo-parser, does not support several messages or destinations per packet
struct message_grammar : boost::spirit::qi::grammar<const char*, boost::spirit::standard::space_type, message()>
{
  message_grammar() : base_type(root)
  {
    namespace qi = boost::spirit::qi;
    namespace phoenix = boost::phoenix;
    using namespace boost::spirit;

    root  = qi::lit('{')
            >> '"' >> "to" >> '"' >> ':' >> channels >> ','
            >> '"' >> "privmsg" >> '"' >> ':' >> '"' >> qi::no_skip[content] >> '"'
            >> '}'
          ;

    channels = channel_str
             | (  '[' >> channel_str % ',' >> ']' )
             ;

    channel_str = '"' >> qi::no_skip[channel] >> '"';

    channel = qi::lit("irc://") >> server >> '/' >> +(qi::char_ - qi::char_('"') - boost::spirit::standard::space);
    server  = +(qi::char_ - qi::char_("/:") - boost::spirit::standard::space) >> port;
    port   %= (':' >> qi::int_)
              | qi::eps [ _val = 6667 ]
            ;
    content = *content_char;
    content_char %= ( qi::lit("\\u") >> qi::uint_parser<unsigned int, 16, 4, 4>() ) [ _val = phoenix::static_cast_<char>(_1) ]
                  | ( qi::lit('\\') >> qi::char_  )
                  | ( qi::char_ - qi::char_('"')  )
                  ;
  }

  boost::spirit::qi::rule<const char*, boost::spirit::standard::space_type, message()> root;
  boost::spirit::qi::rule<const char*, boost::spirit::standard::space_type, std::vector<irc_server_channel>()> channels;
  boost::spirit::qi::rule<const char*, boost::spirit::standard::space_type, irc_server_channel()> channel_str;
  boost::spirit::qi::rule<const char*, irc_server_channel()> channel;
  boost::spirit::qi::rule<const char*, irc_server()> server;
  boost::spirit::qi::rule<const char*, int()> port;
  boost::spirit::qi::rule<const char*, std::string()> content;
  boost::spirit::qi::rule<const char*, char()> content_char;
};

// whole packet must fit in BUFFER_SIZE
void read_irker_packet(int fd)
{
  struct sockaddr_storage addr;
  socklen_t addr_size = sizeof addr;
  char buffer[BUFFER_SIZE];
  std::cout << "reading irker packet" << std::endl;
  ssize_t n = recvfrom(fd, buffer, sizeof buffer, 0, (struct sockaddr*)&addr, &addr_size);
  if(n < 0)
  {
    perror("recvfrom");
    return;
  }

  message msg;
  message_grammar g;
  if(!boost::spirit::qi::phrase_parse((const char*)buffer, (const char*)buffer+n, g, boost::spirit::standard::space, msg))
  {
    std::cout << "invalid message" << std::endl;
    std::cout.write(buffer, n);
    std::cout << std::endl;
    return;
  }

  for(std::vector<irc_server_channel>::const_iterator it = msg.channel.begin(); it != msg.channel.end(); ++it)
    connection_by_server(it->server).privmsg(it->channel, msg.content);
}

int main()
{
  srand(time(0));

  int fd = socket(PF_INET, SOCK_DGRAM, 0);
  if(fd < 0)
  {
    perror("UDP socket");
    exit(1);
  }

  int yes = 1;
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0)
  {
    perror("setsockopt SO_REUSEADDR");
    exit(1);
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0)
  {
    perror("bind");
    exit(1);
  }

  for(;;)
  {
    int maxfd = fd;
    time_t now = time(0);
    time_t min_timeo = TIMEOUT; // minimum amount of time in which a connection will time out
    fd_set readfds;
    fd_set writefds;

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    FD_SET(fd, &readfds);
    for(size_t i=0; i!=connected_servers.size(); ++i)
    {
      int ifd = connected_servers[i].fd;
      if(ifd == -1)
        continue;
      time_t timeo = (now - connected_servers[i].last_activity) >= TIMEOUT ? 0 : (TIMEOUT - (now - connected_servers[i].last_activity));
      maxfd = ifd > maxfd ? ifd : maxfd;
      min_timeo = timeo < min_timeo ? timeo : min_timeo;
      FD_SET(ifd, &readfds);
      if(!connected_servers[i].write_queue.empty())
        FD_SET(ifd, &writefds);
    }

    struct timeval tv;
    memset(&addr, 0, sizeof tv);
    tv.tv_sec = min_timeo;
    ::select(maxfd+1, &readfds, &writefds, NULL, &tv);

    if(FD_ISSET(fd, &readfds))
      read_irker_packet(fd);

    for(size_t i=0; i!=connected_servers.size(); ++i)
    {
      int ifd = connected_servers[i].fd;
      if(ifd != -1)
      {
        if(FD_ISSET(ifd, &readfds))
          connected_servers[i].read();
        if(FD_ISSET(ifd, &writefds))
          connected_servers[i].write();
      }

      // make sure everyone is connected
      connected_servers[i].connect();
    }
  }
}
