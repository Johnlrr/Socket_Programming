#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <cstdint>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 8080
#define BUFFER_SIZE 1024

using namespace std;

struct FileInfo {
    string name;
    int32_t size;  // Use int32_t for consistency with client
};

vector<FileInfo> readFileList(const string& fileName) {
    vector<FileInfo> fileList;
    ifstream file(fileName);
    string line;

    while (getline(file, line)) {
        istringstream iss(line);
        string name;
        int32_t size;
        if (iss >> name >> size) {
            fileList.push_back({ name, size });
        }
    }

    return fileList;
}

void handleClient(SOCKET clientSocket, const vector<FileInfo>& fileList) {
    cout << "Client connected." << endl;

    // Receive client name
    char clientName[BUFFER_SIZE] = { 0 };
    int nameLength = recv(clientSocket, clientName, BUFFER_SIZE, 0);
    string clientNameStr(clientName, nameLength);
    cout << "Client name: " << clientNameStr << endl;

    // Send file list to client
    ostringstream oss;
    for (const auto& file : fileList) {
        oss << file.name << " " << file.size << "MB\n";
    }
    string fileListStr = oss.str();
    send(clientSocket, fileListStr.c_str(), fileListStr.size(), 0);

    while (true) {
        // Receive requested file name from client
        char buffer[BUFFER_SIZE] = { 0 };
        int valread = recv(clientSocket, buffer, BUFFER_SIZE, 0);
        if (valread <= 0) {
            break; // No more requests or error, close connection
        }

        string fileName(buffer, valread);
        ifstream file(fileName, ios::binary);
        if (file.is_open()) {
            file.seekg(0, ios::end);
            int32_t fileSize = static_cast<int32_t>(file.tellg());  // Use int32_t for file size
            file.seekg(0, ios::beg);

            // Send file size in network byte order
            int32_t fileSizeNetworkOrder = htonl(fileSize);
            send(clientSocket, (char*)&fileSizeNetworkOrder, sizeof(fileSizeNetworkOrder), 0);

            // Send file data
            char fileBuffer[BUFFER_SIZE];
            while (!file.eof()) {
                file.read(fileBuffer, BUFFER_SIZE);
                int bytesRead = file.gcount();
                send(clientSocket, fileBuffer, bytesRead, 0);
            }
            file.close();

            cout << "File " << fileName << " has been sent to " << clientNameStr << endl;
        }
        else {
            // File not found, send file size as 0 in network byte order
            int32_t fileSize = 0;
            int32_t fileSizeNetworkOrder = htonl(fileSize);
            send(clientSocket, (char*)&fileSizeNetworkOrder, sizeof(fileSizeNetworkOrder), 0);
        }
    }

    cout << clientNameStr << " disconnected." << endl;
    closesocket(clientSocket);
}

int main() {
    WSADATA wsaData;
    int result;

    // Initialize Winsock
    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        cerr << "Socket creation failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(serverSocket, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        cerr << "Bind failed: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Listen failed: " << WSAGetLastError() << endl;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    vector<FileInfo> fileList = readFileList("file_list.txt");
    cout << "Server is waiting on PORT 8080..." << endl;

    while (true) {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            cerr << "Accept failed: " << WSAGetLastError() << endl;
            closesocket(serverSocket);
            WSACleanup();
            return 1;
        }

        
        // Block second client until finish the first client
        handleClient(clientSocket, fileList);
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}