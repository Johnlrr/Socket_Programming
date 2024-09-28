#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <cstdint>
#include <map>
#include <mutex>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 8080
#define BUFFER_SIZE 1024

using namespace std;

struct FileInfo {
    string name;
    int size;
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

void sendFileChunks(SOCKET clientSocket, const string& clientName, const string& fileName, int chunksToSend, uint32_t& remainingBytes, uint32_t originalFileSize) {
    if (remainingBytes > 0) {
        ifstream fileStream(fileName, ios::binary);
        if (fileStream) {
            fileStream.seekg(originalFileSize - remainingBytes, ios::beg);
            for (int i = 0; i < chunksToSend; ++i) {
                char buffer[BUFFER_SIZE] = { 0 };
                if (remainingBytes <= 0) break;

                fileStream.read(buffer, min(BUFFER_SIZE, remainingBytes));
                int bytesRead = fileStream.gcount();
                if (bytesRead <= 0) break;
                send(clientSocket, buffer, bytesRead, 0);
                remainingBytes -= bytesRead;
            }

            if (remainingBytes <= 0) {
                cout << "Completed sending " << fileName << " to " << clientName << endl;
            }
            fileStream.close();
        }
        else {
            cerr << "Error opening file: " << fileName << endl;
        }
    }
}

void handleClient(SOCKET clientSocket, const vector<FileInfo>& fileList) {
    cout << "Client connected." << endl;

    // Receive client name
    char clientName[BUFFER_SIZE] = { 0 };
    int nameLength = recv(clientSocket, clientName, BUFFER_SIZE, 0);
    if (nameLength == SOCKET_ERROR) {
        cerr << "Error receiving client name: " << WSAGetLastError() << endl;
        closesocket(clientSocket);
        return;
    }
    string clientNameStr(clientName, nameLength);
    cout << "Client name: " << clientNameStr << endl;

    // Send file list to client
    ostringstream oss;
    for (const auto& file : fileList) {
        oss << file.name << " " << file.size << "MB\n";
    }
    string fileListStr = oss.str();
    send(clientSocket, fileListStr.c_str(), fileListStr.size(), 0);

    vector<pair<string, int>> requestedFiles;
    map<string, uint32_t> remainingBytes;
    map<string, uint32_t> originalFileSizes;
    mutex fileMutex;

    while (true) {
        // Listen for a new file request list from the client
        uint32_t numFiles;
        int result = recv(clientSocket, (char*)&numFiles, sizeof(numFiles), 0);
        if (result <= 0) {
            cerr << "Error receiving number of files: " << WSAGetLastError() << endl;
            closesocket(clientSocket);
            return;
        }
        numFiles = ntohl(numFiles);

        {
            lock_guard<mutex> lock(fileMutex);
            for (uint32_t i = 0; i < numFiles; i++) {
                char dataBuffer[BUFFER_SIZE] = { 0 };

                int dataLen = recv(clientSocket, dataBuffer, BUFFER_SIZE, 0);
                if (dataLen <= 0) {
                    cerr << "Error receiving file name and priority\n";
                    closesocket(clientSocket);
                    return;
                }

                string dataReceived(dataBuffer, dataLen);
                // Split the string based on the delimiter "|"
                size_t delimiterPos = dataReceived.find("|");
                if (delimiterPos != string::npos) {
                    string fileName = dataReceived.substr(0, delimiterPos);
                    string priority = dataReceived.substr(delimiterPos + 1);

                    int priorityValue = (priority == "CRITICAL") ? 10 : (priority == "HIGH") ? 4 : 1;
                    requestedFiles.push_back({ fileName, priorityValue });

                    // If the file is new, calculate and send the file size
                    if (remainingBytes.find(fileName) == remainingBytes.end()) {
                        ifstream fileStream(fileName, ios::binary);
                        if (fileStream) {
                            fileStream.seekg(0, ios::end);
                            uint32_t fileSize = fileStream.tellg();
                            originalFileSizes[fileName] = fileSize;
                            remainingBytes[fileName] = fileSize;
                            uint32_t fileSizeNetworkOrder = htonl(fileSize);
                            send(clientSocket, (char*)&fileSizeNetworkOrder, sizeof(fileSizeNetworkOrder), 0);
                        }
                        else {
                            uint32_t fileSize = 0;
                            uint32_t fileSizeNetworkOrder = htonl(fileSize);
                            send(clientSocket, (char*)&fileSizeNetworkOrder, sizeof(fileSizeNetworkOrder), 0);
                        }
                    }
                }
                else {
                    cerr << "Delimiter not found in received data\n";
                }
            }
        }

        // Handle the file transfer logic with the updated list
        thread([&]() {
            while (!requestedFiles.empty()) {
                lock_guard<mutex> lock(fileMutex);
                for (auto& file : requestedFiles) {
                    sendFileChunks(clientSocket, clientNameStr, file.first, file.second, remainingBytes[file.first], originalFileSizes[file.first]);
                }

                requestedFiles.erase(
                    remove_if(requestedFiles.begin(), requestedFiles.end(),
                        [&](const pair<string, int>& file) { return remainingBytes[file.first] <= 0; }),
                    requestedFiles.end()
                );
                this_thread::sleep_for(chrono::milliseconds(100)); // A slight delay to prevent tight loop
            }
            }).detach();
    }

    closesocket(clientSocket);
    cout << clientNameStr << " disconnected.\n";
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
    address.sin_addr.s_addr = htonl(INADDR_ANY);
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
    cout << "Server is waiting on PORT " << PORT << "..." << endl;

    while (true) {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            cerr << "Accept failed: " << WSAGetLastError() << endl;
            closesocket(serverSocket);
            WSACleanup();
            return 1;
        }

        thread clientThread(handleClient, clientSocket, fileList);
        clientThread.detach();
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
