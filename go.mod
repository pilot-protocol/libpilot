module github.com/pilot-protocol/libpilot

go 1.25.12

require (
	github.com/pilot-protocol/common v0.5.5
	github.com/pilot-protocol/handshake v0.2.1
	github.com/pilot-protocol/pilotprotocol v1.10.5
	github.com/pilot-protocol/policy v0.2.2
	github.com/pilot-protocol/runtime v0.3.1
	github.com/pilot-protocol/trustedagents v0.2.3
)

require (
	github.com/coder/websocket v1.8.15 // indirect
	github.com/expr-lang/expr v1.17.8 // indirect
	golang.org/x/sys v0.46.0 // indirect
)

replace github.com/pilot-protocol/pilotprotocol => ../web4

replace github.com/pilot-protocol/common => ../common

replace github.com/pilot-protocol/handshake => ../handshake

replace github.com/pilot-protocol/policy => ../policy

replace github.com/pilot-protocol/runtime => ../runtime

replace github.com/pilot-protocol/skillinject => ../skillinject

replace github.com/pilot-protocol/trustedagents => ../trustedagents

replace github.com/pilot-protocol/rendezvous => ../rendezvous

replace github.com/pilot-protocol/beacon => ../beacon

replace github.com/pilot-protocol/dataexchange => ../dataexchange

replace github.com/pilot-protocol/eventstream => ../eventstream

replace github.com/pilot-protocol/gateway => ../gateway

replace github.com/pilot-protocol/nameserver => ../nameserver

replace github.com/pilot-protocol/webhook => ../webhook

replace github.com/pilot-protocol/updater => ../updater

replace github.com/pilot-protocol/app-store => ../app-store
