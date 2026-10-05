\# IE3090 RemoteOps — IT24101899



\*\*RemoteOps:\*\* A Remote System Monitoring and Management Tool over TCP/IP.



Part 1 — Take-Home Implementation.



\## Personalised values

\- Agent listening port: 9410

\- SID tag: 9981

\- Auth token: OPS-1899

\- Source files: agent\_899.c, controller\_899.c, Makefile\_899

\- Log file: remoteops\_IT24101899.log

\- Storage path: ./agentfiles/IT24101899/



\## Build



&#x20;   make -f Makefile\_899 clean

&#x20;   make -f Makefile\_899



\## Run



Terminal 1:

&#x20;   ./agent\_899



Terminal 2:

&#x20;   ./controller\_899 127.0.0.1



\## Commands

\- AUTH <token>

\- SYSINFO

\- LISTPROC

\- EXEC <DATE|UPTIME|DISKFREE|HOSTNAME|WHOAMI>

\- PUT <name> <size>

\- GET <name>

\- MONITOR START <udp\_port>

\- MONITOR STOP

\- QUIT



\## Author

Kavirathna N R (IT24101899)

