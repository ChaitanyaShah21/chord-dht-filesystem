# AOS Assignment 3 -- Interim Submission

Peer-to-Peer Distributed File Sharing System (Interim)

## Overview

This submission implements: - **Multi-Tracker Synchronization** - **User
and Group Management Commands**

File operations (upload/download, hashing, piece management) will be
implemented in the final submission.

------------------------------------------------------------------------

## Features Implemented

### 1. Multi-Tracker Synchronization

-   Trackers keep connections with peer trackers (`peer_addrs`,
    `peer_sockets`).
-   Any user/group update is broadcast using `SYNC <command>`.
-   If a tracker goes offline and reconnects, it receives missed updates
    from the `update_log`.
-   Clients maintain a list of available trackers and can **switch**
    automatically if the current tracker fails.

### 2. User and Group Management

Implemented commands: - `create_user <user> <pass>` -- Register new
user - `login <user> <pass>` -- Authenticate and login - `logout <user>`
-- Logout user - `create_group <groupid> <owner>` -- Create group -
`join_group <groupid> <user>` -- Request to join group -
`list_requests <groupid> <owner>` -- View pending requests (owner
only) - `accept_request <groupid> <owner> <user>` -- Accept join
request - `list_groups` -- Show all groups - `list_members <groupid>` --
Show members of group - `leave_group <groupid> <user>` -- Leave a group
(if owner leaves, ownership is transferred or group is deleted)

------------------------------------------------------------------------

## Architecture

### Tracker

-   Maintains user database (`users`), online sessions (`online_users`),
    and groups (`groups`).
-   Handles client connections via TCP sockets.
-   Synchronizes state with peer trackers.
-   Logs all updates in `update_log` for replay in case of missed syncs.

### Client

-   Connects to tracker using TCP.
-   Reads commands from user input and sends them to tracker.
-   Handles reconnection: retries the same tracker first, then switches
    to another tracker if available.
-   Updates tracker list when provided by server (`TRACKERS ...`
    message).

------------------------------------------------------------------------

## How to Compile

### Compile Tracker

``` bash
g++ -std=c++17 -pthread tracker.cpp -o tracker
```

### Compile Client

``` bash
g++ -std=c++17 -pthread client.cpp -o client
```

------------------------------------------------------------------------

## How to Run

### Start Tracker(s)

Run at least one tracker:

``` bash
./tracker <listen_port>
```

For multi-tracker setup:

``` bash
./tracker 5001 127.0.0.1:5002
./tracker 5002 127.0.0.1:5001
```

### Start Client

``` bash
./client <tracker_ip> <tracker_port>
```

Example:

``` bash
./client 127.0.0.1 5001
```

------------------------------------------------------------------------

## Example Commands (Client Side)

``` text
create_user alice pass123
login alice pass123
create_group g1 alice
join_group g1 alice
list_groups
logout alice
```

------------------------------------------------------------------------

## Current Limitations

-   File sharing (`upload`, `download`, SHA1 hashing) is **not yet
    implemented** (to be completed in final submission).
-   Security: authentication tokens/sessions not implemented (any
    logged-in user can act for others).
-   No persistence: tracker state is in-memory only.
-   Only basic error handling implemented.

------------------------------------------------------------------------

