# netlink nl80211 example utility to get client info connected to wireless AP 
Almost no dependencies utility, except `libnl-3`.

## Howto build
libnl-3-dev libnl-genl-3-dev packets should be installed.
```
git clone <repo_url>
cd <repo_dir>
make station_get
```
Binary is in the `build` directory

## Howto use
```
┌──(ru㉿kali)-[~/netlink_station_get]
└─$ make
gcc -Wall -O2 -I./ -I/usr/include/libnl3  main.c -o build/station_get -lnl-3 -lnl-genl-3

┌──(ru㉿kali)-[~/netlink_station_get]
└─$ ./build/station_get dev wlan0
nl80211 driver id: 31
attr. type: 3 NL80211_ATTR_IFINDEX
attr. type: 6 NL80211_ATTR_MAC
attr. type: 21 NL80211_ATTR_STA_INFO
attr. type: 46 NL80211_ATTR_GENERATION
dev idx: 9 name: wlan0
mac: 50:3D:C6:54:77:C1

        inactive time:  304 ms
        signal:         -49 dBm
        current time:   1661597179316 ms

┌──(ru㉿kali)-[~/netlink_station_get]
└─$ ./build/station_get dev wlan0 mac 50:3D:C6:54:77:C1
nl80211 driver id: 31
attr. type: 3 NL80211_ATTR_IFINDEX
attr. type: 6 NL80211_ATTR_MAC
attr. type: 21 NL80211_ATTR_STA_INFO
attr. type: 46 NL80211_ATTR_GENERATION
dev idx: 9 name: wlan0
mac: 50:3D:C6:54:77:C1

        inactive time:  448 ms
        rx packets:     588
        tx packets:     149
        tx failed:      0
        signal:         -46 dBm
        current time:   1661597189488 ms
```
