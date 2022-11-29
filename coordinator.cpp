#include "CurlEasyPtr.h"
#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

using namespace std::literals;

#define BACKLOG 10   // how many pending connections queue will hold

void sigchld_handler(int s) {
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;
    while(waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


/// Leader process that coordinates workers. Workers connect on the specified port
/// and the coordinator distributes the work of the CSV file list.
/// Example:
///    ./coordinator http://example.org/filelist.csv 4242
int main(int argc, char* argv[]) {
   std::cout << "Coordinator started" << std::endl;
   if (argc != 3) {
      std::cerr << "Usage: " << argv[0] << " <URL to csv list> <listen port>" << std::endl;
      return 1;
   }

   // TODO:
   //    1. Allow workers to connect
   //       socket(), bind(), listen(), accept(), see: https://beej.us/guide/bgnet/html/#system-calls-or-bust
   //    2. Distribute the following work among workers
   //       send() them some work
   //    3. Collect all results
   //       recv() the results
   // Hint: Think about how you track which worker got what work

   int PORT = atoi(argv[2]);
   int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
   struct addrinfo hints, *servinfo, *p;
   struct sockaddr_storage their_addr; // connector's address information
   socklen_t sin_size;
   struct sigaction sa;
   int yes=1;
   char s[INET6_ADDRSTRLEN];
   int rv;

   memset(&hints, 0, sizeof hints);
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE; // use my IP

   if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
      return 1;
   }

   // loop through all the results and bind to the first we can
   for(p = servinfo; p != NULL; p = p->ai_next) {
      if ((sockfd = socket(p->ai_family, p->ai_socktype,
               p->ai_protocol)) == -1) {
         perror("server: socket");
         continue;
      }

      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
               sizeof(int)) == -1) {
         perror("setsockopt");
         exit(1);
      }

      if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
         close(sockfd);
         perror("server: bind");
         continue;
      }

      break;
   }

   freeaddrinfo(servinfo); // all done with this structure

   if (p == NULL)  {
      fprintf(stderr, "server: failed to bind\n");
      exit(1);
   }

   if (listen(sockfd, BACKLOG) == -1) {
      perror("listen");
      exit(1);
   }

   sa.sa_handler = sigchld_handler; // reap all dead processes
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART;
   if (sigaction(SIGCHLD, &sa, NULL) == -1) {
      perror("sigaction");
      exit(1);
   }

   printf("server: waiting for connections...\n");

   while(1) {  // main accept() loop
      sin_size = sizeof their_addr;
      new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
      if (new_fd == -1) {
         perror("accept");
         continue;
      }

      inet_ntop(their_addr.ss_family,
         get_in_addr((struct sockaddr *)&their_addr),
         s, sizeof s);
      printf("server: got connection from %s\n", s);

      if (!fork()) { // this is the child process
         close(sockfd); // child doesn't need the listener
         if (send(new_fd, "Hello, world!", 13, 0) == -1)
               perror("send");
         close(new_fd);
         exit(0);
      }
      close(new_fd);  // parent doesn't need this
   }

   auto curlSetup = CurlGlobalSetup();

   auto listUrl = std::string(argv[1]);

   // Download the file list
   auto curl = CurlEasyPtr::easyInit();
   curl.setUrl(listUrl);
   auto fileList = curl.performToStringStream();

   size_t result = 0;
   // Iterate over all files
   for (std::string url; std::getline(fileList, url, '\n');) {
      curl.setUrl(url);
      // Download them
      auto csvData = curl.performToStringStream();
      for (std::string row; std::getline(csvData, row, '\n');) {
         auto rowStream = std::stringstream(std::move(row));

         // Check the URL in the second column
         unsigned columnIndex = 0;
         for (std::string column; std::getline(rowStream, column, '\t'); ++columnIndex) {
            // column 0 is id, 1 is URL
            if (columnIndex == 1) {
               // Check if URL is "google.ru"
               auto pos = column.find("://"sv);
               if (pos != std::string::npos) {
                  auto afterProtocol = std::string_view(column).substr(pos + 3);
                  if (afterProtocol.starts_with("google.ru/"))
                     ++result;
               }
               break;
            }
         }
      }
   }

   std::cout << result << std::endl;

   return 0;
}
