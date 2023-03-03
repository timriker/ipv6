# IPv6 testing

Some reference material:

https://www.rfc-editor.org/rfc/rfc4038

https://beej.us/guide/bgnet/html/

https://www.kame.net/newsletter/19980604/

https://www.hpc.mil/images/hpcdocs/ipv6/porting\_ipv4tov6\_johar\_2002.pdf

## Testing

server listens on tcp and udp given the ip and port on the command line.
It echos a hexdump of whatever is set to it. Test server.c by compiling:

g++ -o server server.c

Then run:

$ ./server :: 8000

then netcat to send data:

$ nc -N -i 1 -u localhost 8000 < README.md

## formatting

style --style=java -nxjQ --convert-tabs --max-code-length=120 *.c