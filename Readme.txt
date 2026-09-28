How to Build:
Compile everything with a single command: (in bash)
make


Remove compiled binaries:

make clean

To RUn:
Step 1: Ensure Configuration Files are Present
Make sure your configuration files match your executable directory:

config_master.cfg (for the token master controller)

config_node.cfg (for the token node daemon)

Step 2: Start the Token Master Controller
Open your first terminal window and run the master executable:

./token_master

What it does: It loads config_master.cfg, listens for incoming client/bridge 
requests on Port P1 (e.g., 8090), coordinates cluster-wide active/standby status, 
and balances token tasks across available token_node instances.

Step 3: Start the Token Node Daemon(s)
Open a second terminal window and run the token node executable, passing its configuration file as a command-line argument:

./token_node config_node.cfg

(If you are running multiple token nodes on different ports, create separate c
onfig files like config_node_9021.cfg and run: ./token_node config_node_9021.cfg)

What it does: It binds to the specified port (e.g., 9020), 
handles multi-threaded generate_token and check_token_expiration requests, 
logs metrics to PostgreSQL, and executes periodic heartbeats every 5 minutes.

Step 4: Start the Node.js API Bridge (Optional)
If you want to communicate with the cluster from your client application or 
browser UI via the Node.js API bridge:

node token_bridge.js


What it does: Runs the Express server on port 3007, 
exposing endpoints (/api/tokens/generate and /api/tokens/check) 
that forward payloads to the C++ token_master controller.


