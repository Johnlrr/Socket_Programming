#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <signal.h>
#pragma comment(lib, "Ws2_32.lib")

#define PORT 8080
#define BUFFER_SIZE 1024
#define INPUT_FILE "input.txt"
#define DOWNLOADED_FILE_LIST "downloaded_files.txt"
volatile bool keepRunning = true;

using namespace std;

// Function to download a file from the server
void downloadFile(SOCKET socket, const string& fileName) {
    // Send requested file name to server
    send(socket, fileName.c_str(), fileName.size(), 0);

    // Receive file size
    uint32_t fileSize;
    int result = recv(socket, (char*)&fileSize, sizeof(fileSize), 0);
    if (result <= 0) {
        cerr << "Error receiving file size for " << fileName << "\n";
        return;
    }
    fileSize = ntohl(fileSize);  // Convert from network byte order to host byte order
    if (fileSize == 0) {
        cerr << "File " << fileName << " not found on server or invalid file size.\n";
        return;
    }

    cout << "File size of " << fileName << ": " << fileSize << " bytes\n";

    // Receive file data and save to output folder
    ofstream file("output/" + fileName, ios::binary);
    if (!file.is_open()) {
        cerr << "Unable to open file for writing: output/" << fileName << "\n";
        return;
    }

    char buffer[BUFFER_SIZE];
    uint32_t totalBytesRead = 0;
    uint32_t previousPercentage = 0; // To track the previous percentage displayed

    while (totalBytesRead < fileSize) {
        int bytesRead = recv(socket, buffer, BUFFER_SIZE, 0);
        if (bytesRead <= 0) {
            cerr << "Connection lost or error while receiving " << fileName << ".\n";
            break;
        }
        file.write(buffer, bytesRead);
        totalBytesRead += bytesRead;

        // Calculate the current percentage
        uint32_t percentage = (totalBytesRead * 100) / fileSize;

        // Only update percentage if it has changed
        if (percentage != previousPercentage) {
            cout << "Downloading " << fileName << " .... " << percentage << "%" << endl;
            previousPercentage = percentage;
        }
    }
    file.close();
    if (totalBytesRead == fileSize) {
        cout << "Completely downloaded " << fileName << endl;
    }
    else {
        cerr << "Failed to download " << fileName << endl;
    }
}


// Function to read the list of files to download
vector<string> readFileList(const string& filename, const set<string>& downloadedFiles) {
    vector<string> fileList;
    ifstream file(filename);
    string line;

    while (getline(file, line)) {
        if (!line.empty() && downloadedFiles.find(line) == downloadedFiles.end()) // Check if line is not empty
            // and line is not in downloaded_files.txt, mean that file is not downloaded
        {
            fileList.push_back(line);
        }
    }

    return fileList;
}


// Function to read already downloaded files
set<string> readDownloadedFiles() {
    set<string> downloadedFiles;
    ifstream file(DOWNLOADED_FILE_LIST);
    string line;

    while (getline(file, line)) {
        if (!line.empty()) {
            downloadedFiles.insert(line);
        }
    }

    return downloadedFiles;
}

// Function to save downloaded file names
void saveDownloadedFile(const string& fileName) {
    ofstream file(DOWNLOADED_FILE_LIST, ios::app);
    if (file.is_open()) {
        file << fileName << endl;
        file.close();
    }
    else {
        cerr << "Unable to open file: " << DOWNLOADED_FILE_LIST << endl;
    }
}
void signal_callback_handler(int signum) {
    cout << "Exit..." << endl;
    keepRunning = false;
    exit(signum);
}
int main() {
    signal(SIGINT, signal_callback_handler);
    WSADATA wsaData;
    int result;

    // Initialize Winsock
    result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        cerr << "Socket creation error" << endl;
        WSACleanup();
        return -1;
    }

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        cerr << "Invalid address/ Address not supported" << endl;
        return -1;
    }

    // Connect to server
    if (connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        cerr << "Connection Failed" << endl;
        return -1;
    }

    // Get client name
    string clientName;
    cout << "Enter your name: ";
    getline(cin, clientName);

    // Send client name to server
    send(sock, clientName.c_str(), clientName.size(), 0);

    // Receive file list from server
    char buffer[BUFFER_SIZE] = { 0 };
    int valread = recv(sock, buffer, BUFFER_SIZE, 0);
    if (valread > 0) {
        cout << "Available files:\n" << string(buffer, valread) << endl;
    }

    set<string> downloadedFiles = readDownloadedFiles();


    while (keepRunning) {
        vector<string> filesToDownload = readFileList(INPUT_FILE, downloadedFiles);

        for (const auto& fileName : filesToDownload) {
            downloadFile(sock, fileName);

            // Save the file name to downloaded_files.txt
            saveDownloadedFile(fileName);
            downloadedFiles.insert(fileName);
        }

        if (keepRunning) {
            cout << "Press 'Ctrl + C' to quit or any other key to check for new files...\n";
            string choice;
            cin >> choice;
        }

    }

    closesocket(sock);
    WSACleanup();
    return 0;
}