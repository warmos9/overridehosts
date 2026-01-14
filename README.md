# overridehosts
Simple wrapper binary to inject custom hosts into subprocess without modifying /etc/hosts. Works even without root.

## Compilation
```
g++ -O2 -std=c++17 -fPIC -shared -o liboverridehosts.so liboverridehosts.cpp -ldl
g++ -O2 -std=c++17 -o overridehosts overridehosts.cpp
```

## Usage
Single host override to ping
`./overridehosts "test:192.168.0.1" -- ping test`

Single host override using env
```
export OVERRIDEHOSTS="test:192.168.0.1"
overridehosts -- ping test
```

Hosts override into shell, then adjust inside shell
```
export OVERRIDEHOSTS="test:192.168.0.1"
overridehosts -- bash
ping -c 1 test
export OVERRIDEHOSTS="test:192.168.0.2"
ping -c 1 test
```
