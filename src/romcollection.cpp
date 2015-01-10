/***
 * Copyright (c) 2013, Presence
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the organization nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ***/

#include "romcollection.h"


RomCollection::RomCollection(QStringList fileTypes, QStringList romPaths, QWidget *parent) : QObject(parent)
{
    this->fileTypes = fileTypes;
    this->romPaths = romPaths;
    this->parent = parent;

    setupDatabase();
}


Rom RomCollection::addRom(QByteArray *romData, QString fileName, QString directory, QString zipFile,
                          QSqlQuery query, bool ddRom)
{
    Rom currentRom;

    currentRom.fileName = fileName;
    currentRom.directory = directory;

    if (ddRom)
        currentRom.internalName = "";
    else
        currentRom.internalName = QString(romData->mid(32, 20)).trimmed();

    currentRom.romMD5 = QString(QCryptographicHash::hash(*romData,
                                QCryptographicHash::Md5).toHex());
    currentRom.zipFile = zipFile;
    currentRom.sortSize = romData->size();

    query.bindValue(":filename",      currentRom.fileName);
    query.bindValue(":directory",     currentRom.directory);
    query.bindValue(":internal_name", currentRom.internalName);
    query.bindValue(":md5",           currentRom.romMD5);
    query.bindValue(":zip_file",      currentRom.zipFile);
    query.bindValue(":size",          currentRom.sortSize);

    if (ddRom)
        query.bindValue(":dd_rom", 1);
    else
        query.bindValue(":dd_rom", 0);

    query.exec();

    if (!ddRom)
        initializeRom(&currentRom, false);

    return currentRom;
}


void RomCollection::addRoms()
{
    emit updateStarted();


    //Count files so we know how to setup the progress dialog
    int totalCount = 0;

    foreach (QString romPath, romPaths) {
        QDir romDir(romPath);

        if (romDir.exists()) {
            QStringList files = romDir.entryList(fileTypes, QDir::Files | QDir::NoSymLinks);
            totalCount += files.size();
        }
    }


    database.open();

    QSqlQuery query("DELETE FROM rom_collection", database);
    query.prepare(QString("INSERT INTO rom_collection ")
                  + "(filename, directory, internal_name, md5, zip_file, size, dd_rom) "
                  + "VALUES (:filename, :directory, :internal_name, :md5, :zip_file, :size, :dd_rom)");

    scrapper = new TheGamesDBScrapper(parent);

    QList<Rom> roms;
    QList<Rom> ddRoms;

    if (totalCount != 0) {
        int count = 0;
        setupProgressDialog(totalCount);

        foreach (QString romPath, romPaths)
        {
            QDir romDir(romPath);
            QStringList files = romDir.entryList(fileTypes, QDir::Files | QDir::NoSymLinks);

            int romCount = 0;

            foreach (QString fileName, files)
            {
                QString completeFileName = romDir.absoluteFilePath(fileName);
                QFile file(completeFileName);

                //If file is a zip file, extract info from any zipped ROMs
                if (QFileInfo(file).suffix().toLower() == "zip") {
                    foreach (QString zippedFile, getZippedFiles(completeFileName))
                    {
                        //check for ROM files
                        QByteArray *romData = getZippedRom(zippedFile, completeFileName);

                        if (romData->left(4).toHex() == "80371240") { //Z64 ROM
                            roms.append(addRom(romData, zippedFile, romPath, fileName, query));
                            romCount++;
                        } else if (romData->left(4).toHex() == "e848d316") { //64DD ROM
                            ddRoms.append(addRom(romData, zippedFile, romPath, fileName, query, true));
                            romCount++;
                        }

                        delete romData;
                    }
                } else { //Just a normal file
                    file.open(QIODevice::ReadOnly);
                    QByteArray *romData = new QByteArray(file.readAll());
                    file.close();

                    if (romData->left(4).toHex() == "80371240") { //Z64 ROM
                        roms.append(addRom(romData, fileName, romPath, "", query));
                        romCount++;
                    } else if (romData->left(4).toHex() == "e848d316") { //64DD ROM
                        ddRoms.append(addRom(romData, fileName, romPath, "", query, true));
                        romCount++;
                    }

                    delete romData;
                }

                count++;
                progress->setValue(count);
            }

            if (romCount == 0)
                QMessageBox::warning(parent, "Warning", "No ROMs found in " + romPath + ".");
        }

        progress->close();
    } else {
        QMessageBox::warning(parent, "Warning", "No ROMs found.");
    }

    delete scrapper;

    database.close();

    //Emit signals for regular roms
    qSort(roms.begin(), roms.end(), romSorter);

    for (int i = 0; i < roms.size(); i++)
        emit romAdded(&roms[i], i);

    //Emit signals for 64DD roms
    qSort(ddRoms.begin(), ddRoms.end(), romSorter);

    for (int i = 0; i < ddRoms.size(); i++)
        emit ddRomAdded(&ddRoms[i]);

    emit updateEnded(roms.size());
}


void RomCollection::cachedRoms(bool imageUpdated)
{
    emit updateStarted(imageUpdated);

    database.open();
    QSqlQuery query(QString("SELECT filename, directory, md5, internal_name, zip_file, size, dd_rom ")
                    + "FROM rom_collection", database);

    query.last();
    int romCount = query.at() + 1;
    query.seek(-1);

    if (romCount == -1) { //Nothing cached so try adding ROMs instead
        addRoms();
        return;
    }

    QList<Rom> roms;
    QList<Rom> ddRoms;

    int count = 0;
    bool showProgress = false;
    QTime checkPerformance;

    while (query.next())
    {
        Rom currentRom;

        currentRom.fileName = query.value(0).toString();
        currentRom.directory = query.value(1).toString();
        currentRom.romMD5 = query.value(2).toString();
        currentRom.internalName = query.value(3).toString();
        currentRom.zipFile = query.value(4).toString();
        currentRom.sortSize = query.value(5).toInt();
        int ddRom = query.value(6).toInt();

        //Check performance of adding first item to see if progress dialog needs to be shown
        if (count == 0) checkPerformance.start();

        if (ddRom == 1)
            ddRoms.append(currentRom);
        else {
            initializeRom(&currentRom, true);
            roms.append(currentRom);
        }

        if (count == 0) {
            int runtime = checkPerformance.elapsed();

            //check if operation expected to take longer than two seconds
            if (runtime * romCount > 2000) {
                setupProgressDialog(romCount);
                showProgress = true;
            }
        }

        count++;

        if (showProgress)
            progress->setValue(count);
    }

    database.close();

    if (showProgress)
        progress->close();

    //Emit signals for regular roms
    qSort(roms.begin(), roms.end(), romSorter);

    for (int i = 0; i < roms.size(); i++)
        emit romAdded(&roms[i], i);

    //Emit signals for 64DD roms
    qSort(ddRoms.begin(), ddRoms.end(), romSorter);

    for (int i = 0; i < ddRoms.size(); i++)
        emit ddRomAdded(&ddRoms[i]);

    emit updateEnded(roms.size(), true);
}


QStringList RomCollection::getFileTypes(bool archives)
{
    QStringList returnList = fileTypes;

    if (!archives)
        returnList.removeOne("*.zip");

    return returnList;
}


void RomCollection::initializeRom(Rom *currentRom, bool cached)
{
    QSettings *romCatalog = new QSettings(parent);
    QString catalogFile = SETTINGS.value("Paths/catalog", "").toString();

    QDir romDir(currentRom->directory);

    //Default text for GoodName to notify user
    currentRom->goodName = "Requires catalog file";
    currentRom->imageExists = false;

    bool getGoodName = false;
    if (QFileInfo(catalogFile).exists()) {
        romCatalog = new QSettings(catalogFile, QSettings::IniFormat, parent);
        getGoodName = true;
    }

    QFile file(romDir.absoluteFilePath(currentRom->fileName));

    currentRom->romMD5 = currentRom->romMD5.toUpper();
    currentRom->baseName = QFileInfo(file).completeBaseName();
    currentRom->size = QObject::tr("%1 MB").arg((currentRom->sortSize + 1023) / 1024 / 1024);

    if (getGoodName) {
        //Join GoodName on ", ", otherwise entries with a comma won't show
        QVariant goodNameVariant = romCatalog->value(currentRom->romMD5+"/GoodName","Unknown ROM");
        currentRom->goodName = goodNameVariant.toStringList().join(", ");

        QStringList CRC = romCatalog->value(currentRom->romMD5+"/CRC","").toString().split(" ");

        if (CRC.size() == 2) {
            currentRom->CRC1 = CRC[0];
            currentRom->CRC2 = CRC[1];
        }

        QString newMD5 = romCatalog->value(currentRom->romMD5+"/RefMD5","").toString();
        if (newMD5 == "")
            newMD5 = currentRom->romMD5;

        currentRom->players = romCatalog->value(newMD5+"/Players","").toString();
        currentRom->saveType = romCatalog->value(newMD5+"/SaveType","").toString();
        currentRom->rumble = romCatalog->value(newMD5+"/Rumble","").toString();
    }

    if (!cached && SETTINGS.value("Other/downloadinfo", "").toString() == "true") {
        if (currentRom->goodName != "Unknown ROM" && currentRom->goodName != "Requires catalog file") {
            scrapper->downloadGameInfo(currentRom->romMD5, currentRom->goodName);
        } else {
            //tweak internal name by adding spaces to get better results
            QString search = currentRom->internalName;
            search.replace(QRegExp("([a-z])([A-Z])"),"\\1 \\2");
            search.replace(QRegExp("([^ \\d])(\\d)"),"\\1 \\2");
            scrapper->downloadGameInfo(currentRom->romMD5, search);
        }

    }

    if (SETTINGS.value("Other/downloadinfo", "").toString() == "true") {
        QString cacheDir = getDataLocation() + "/cache";

        QString dataFile = cacheDir + "/" + currentRom->romMD5.toLower() + "/data.xml";
        QFile file(dataFile);

        file.open(QIODevice::ReadOnly);
        QString dom = file.readAll();
        file.close();

        QDomDocument xml;
        xml.setContent(dom);
        QDomNode game = xml.elementsByTagName("Game").at(0);

        //Remove any non-standard characters
        QString regex = "[^A-Za-z 0-9 \\.,\\?'""!@#\\$%\\^&\\*\\(\\)-_=\\+;:<>\\/\\\\|\\}\\{\\[\\]`~]*";

        currentRom->gameTitle = game.firstChildElement("GameTitle").text().remove(QRegExp(regex));
        if (currentRom->gameTitle == "") currentRom->gameTitle = "Not found";

        currentRom->releaseDate = game.firstChildElement("ReleaseDate").text();

        //Fix missing 0's in date
        currentRom->releaseDate.replace(QRegExp("^(\\d)/(\\d{2})/(\\d{4})"), "0\\1/\\2/\\3");
        currentRom->releaseDate.replace(QRegExp("^(\\d{2})/(\\d)/(\\d{4})"), "\\1/0\\2/\\3");
        currentRom->releaseDate.replace(QRegExp("^(\\d)/(\\d)/(\\d{4})"), "0\\1/0\\2/\\3");

        currentRom->sortDate = currentRom->releaseDate;
        currentRom->sortDate.replace(QRegExp("(\\d{2})/(\\d{2})/(\\d{4})"), "\\3-\\1-\\2");

        currentRom->overview = game.firstChildElement("Overview").text().remove(QRegExp(regex));
        currentRom->esrb = game.firstChildElement("ESRB").text();

        int count = 0;
        QDomNode genreNode = game.firstChildElement("Genres").firstChild();
        while(!genreNode.isNull())
        {
            if (count != 0)
                currentRom->genre += "/" + genreNode.toElement().text();
            else
                currentRom->genre = genreNode.toElement().text();

            genreNode = genreNode.nextSibling();
            count++;
        }

        currentRom->publisher = game.firstChildElement("Publisher").text();
        currentRom->developer = game.firstChildElement("Developer").text();
        currentRom->rating = game.firstChildElement("Rating").text();

        QString imageFile = getDataLocation() + "/cache/"
                            + currentRom->romMD5.toLower() + "/boxart-front.jpg";
        QFile cover(imageFile);

        if (cover.exists()) {
            currentRom->image.load(imageFile);
            currentRom->imageExists = true;
        }
    }
}


void RomCollection::setupDatabase()
{
    int dbVersion = 1;

    database = QSqlDatabase::addDatabase("QSQLITE");
    database.setDatabaseName(getDataLocation() + "/cen64-qt.sqlite");

    if (!database.open())
        QMessageBox::warning(parent, "Database Not Loaded",
                             "Could not connect to Sqlite database. Application may misbehave.");

    QSqlQuery version = database.exec("PRAGMA user_version");
    version.next();

    if (version.value(0).toInt() != dbVersion) { //old database version, reset rom_collection
        version.finish();

        database.exec("DROP TABLE rom_collection");
        database.exec("PRAGMA user_version = " + QString::number(dbVersion));
    }

    database.exec(QString()
                    + "CREATE TABLE IF NOT EXISTS rom_collection ("
                        + "rom_id INTEGER PRIMARY KEY ASC, "
                        + "filename TEXT NOT NULL, "
                        + "directory TEXT NOT NULL, "
                        + "md5 TEXT NOT NULL, "
                        + "internal_name TEXT, "
                        + "zip_file TEXT, "
                        + "size INTEGER, "
                        + "dd_rom INTEGER)");

    database.close();
}


void RomCollection::setupProgressDialog(int size)
{
    progress = new QProgressDialog("Loading ROMs...", "Cancel", 0, size, parent);
#if QT_VERSION >= 0x050000
    progress->setWindowFlags(progress->windowFlags() & ~Qt::WindowCloseButtonHint);
    progress->setWindowFlags(progress->windowFlags() & ~Qt::WindowMinimizeButtonHint);
    progress->setWindowFlags(progress->windowFlags() & ~Qt::WindowContextHelpButtonHint);
#else
    progress->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
#endif
    progress->setCancelButton(0);
    progress->setWindowModality(Qt::WindowModal);

    progress->show();
}


void RomCollection::updatePaths(QStringList romPaths)
{
    this->romPaths = romPaths;
}
