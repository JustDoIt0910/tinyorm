//
// Created by zr on 22-10-27.
//

#ifndef __ORM_MIGRATE_HPP__
#define __ORM_MIGRATE_HPP__

#include <vector>
#include "model.hpp"
#include "dataloader.hpp"
#include "mysql4cpp/connectionpool.h"
#include "mysql4cpp/sqlconn.h"
#include "mysql4cpp/databasemetadata.h"
#include "spdlog/spdlog.h"
#include <sstream>

namespace orm {
    struct Cmd
    {
        enum Action
        {
            MIGACTION_CREATE_TABLE,
            MIGACTION_ADD_COLUMN,
            MIGACTION_CHANGE_COLUMN,
            MIGACTION_ADD_INDEX,
            MIGACTION_CHANGE_TABLENAME,
            MIGACTION_DROP_INDEX
        } action;
        int priority;
        std::string targetName;
        std::string prevTargetName;

        Cmd(const std::string& tar, Action ac, const std::string& prevTar = ""):
                targetName(tar), action(ac), prevTargetName(prevTar)
        {
            switch(action)
            {
                case MIGACTION_CREATE_TABLE:
                case MIGACTION_CHANGE_TABLENAME:
                    priority = 1;
                    break;
                case MIGACTION_ADD_COLUMN:
                case MIGACTION_CHANGE_COLUMN:
                    priority = 2;
                    break;
                case MIGACTION_DROP_INDEX:
                    priority = 3;
                    break;
                case MIGACTION_ADD_INDEX:
                    priority = 4;
            }
        }

        bool operator<(const Cmd& cmd)
        {
            return priority < cmd.priority;
        }
    };

    template<typename T>
    class Migrator
    {
    public:
        Migrator(Model<T>* _model);
        void addCommand(const Cmd& cmd);
        virtual void migrate() = 0;
        virtual ~Migrator();

    protected:
        SqlConn conn;
        Model<T>* model;
        std::vector<Cmd> cmds;
    };

    template<typename T>
    Migrator<T>::Migrator(Model<T>* _model): model(_model)
    {
        ConnectionPool* pool = ConnectionPool::getInstance();
        conn = pool->getConn();
    }

    template<typename T>
    void Migrator<T>::addCommand(const Cmd& cmd)
    {
        cmds.push_back(cmd);
    }

    template<typename T>
    Migrator<T>::~Migrator()
    {
        conn.close();
        spdlog::info("Model migrated");
    }

    template<typename T>
    class MysqlMigrator: public Migrator<T>
    {
    public:
        MysqlMigrator(Model<T>* _model);
        void migrate();
    };

    template<typename T>
    MysqlMigrator<T>::MysqlMigrator(Model<T> *_model): Migrator<T>(_model) {}

    template<typename T>
    void MysqlMigrator<T>::migrate()
    {
        sort(this->cmds.begin(), this->cmds.end());
        std::stringstream sql;
        for(const Cmd& cmd: this->cmds)
        {
            switch (cmd.action)
            {
                //??????????????????
                case Cmd::MIGACTION_CHANGE_TABLENAME:
                    sql << "ALTER TABLE " << cmd.prevTargetName << " RENAME AS " << cmd.targetName;
                    spdlog::info(sql.str());
                    break;
                //????????????
                case Cmd::MIGACTION_CREATE_TABLE:
                    sql << "CREATE TABLE " << this->model->getTableName() << "(";
                    for(int i = 0; i < this->model->getAllMetadata().size(); i++)
                    {
                        if(i > 0)
                            sql << ", ";
                        sql << this->model->getAllMetadata()[i].getColumnName() << " "
                        << this->model->getAllMetadata()[i].getColumnType();
                        if(this->model->getAllMetadata()[i].isPk)
                        {
                            sql << " PRIMARY KEY";
                            if(this->model->getAllMetadata()[i].autoInc)
                                sql << " AUTO_INCREMENT";
                        }
                        else if(this->model->getAllMetadata()[i].notNull)
                            sql << " NOT NULL";
                        if(this->model->getAllMetadata()[i]._default.length() > 0)
                            sql << " DEFAULT " << this->model->getAllMetadata()[i]._default;
                    }
                    for(auto it = this->model->getIndices().cbegin(); it != this->model->getIndices().cend(); it++)
                    {
                        if(it->first == "PRIMARY")
                            continue;
                        sql << ", ";
                        if(it->second.type == Index::INDEX_TYPE_MUL)
                            sql << "INDEX ";
                        else
                            sql << "UNIQUE ";
                        sql << it->second.name << "(";
                        for(int c = 0; c < it->second.columns.size(); c++)
                        {
                            if(c > 0)
                                sql << ", ";
                            sql << it->second.columns[c];
                        }
                        sql << ")";
                    }
                    sql << ")";
                    spdlog::info(sql.str());
                    break;
                //????????????????????????
                case Cmd::MIGACTION_CHANGE_COLUMN:
                case Cmd::MIGACTION_ADD_COLUMN:
                    if(cmd.action == Cmd::MIGACTION_CHANGE_COLUMN)
                        sql << "ALTER TABLE " << this->model->getTableName() << " CHANGE "
                        << ((cmd.prevTargetName.length() == 0) ? cmd.targetName : cmd.prevTargetName) << " ";
                    else
                        sql << "ALTER TABLE " << this->model->getTableName() << " ADD COLUMN ";

                    sql<< cmd.targetName << " " << this->model->getMetadata(cmd.targetName).getColumnType();
                    if(this->model->getMetadata(cmd.targetName).notNull)
                        sql << " NOT NULL ";
                    if(this->model->getMetadata(cmd.targetName)._default.length() > 0)
                        sql << " DEFAULT " << this->model->getMetadata(cmd.targetName)._default;
                    sql << this->model->getMetadata(cmd.targetName).extra;
                    if(this->model->getMetadata(cmd.targetName).autoInc)
                        sql << " AUTO_INCREMENT ";
                    spdlog::info(sql.str());
                    break;
                //??????????????????
                case Cmd::MIGACTION_DROP_INDEX:
                    if(cmd.targetName == "PRIMARY")
                        sql << "ALTER TABLE " << this->model->getTableName() << " DROP PRIMARY KEY";
                    else
                        sql << "ALTER TABLE " << this->model->getTableName() << " DROP INDEX " << cmd.targetName;
                    spdlog::info(sql.str());
                    break;
                //??????????????????
                case Cmd::MIGACTION_ADD_INDEX:
                    sql << "ALTER TABLE " << this->model->getTableName() << " ADD ";
                    if(this->model->getIndex(cmd.targetName).type == Index::INDEX_TYPE_MUL)
                        sql << "INDEX " << cmd.targetName << "(";
                    else if(this->model->getIndex(cmd.targetName).type == Index::INDEX_TYPE_UNI)
                        sql << "UNIQUE " << cmd.targetName << "(";
                    else
                        sql << "PRIMARY KEY ";
                    for(int i = 0; i < this->model->getIndex(cmd.targetName).columns.size(); i++)
                    {
                        if(i > 0)
                            sql << ", ";
                        sql << this->model->getIndex(cmd.targetName).columns[i];
                    }
                    sql << ")";
                    spdlog::info(sql.str());
                    break;
            }
            this->conn.executeUpdate(sql.str());
            //??????sql. ???????????????????????????
            std::stringstream().swap(sql);
        }
    }

    template<typename T>
    void Model<T>::AutoMigrate()
    {
        MysqlMigrator<T> migrator(this);
        DataLoader::loadFromFile(db, getClassName(), tablenamePrev, fieldsPrev, indicesPrev, columnToIndexPrev);
        spdlog::info("Model {} migrating...", getClassName());

        //?????????????????????????????????model???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
        if(tablenamePrev.length() == 0)
        {
            //?????????????????????????????????????????????????????????????????????????????????????????????????????????
            DataLoader::loadFromDB(fieldsPrev, indicesPrev, columnToIndexPrev, db, getTableName(), conn);
            //?????????????????????????????????????????????????????????????????????????????????????????????????????????
            if(fieldsPrev.size() == 0)
            {
                DataLoader::loadFromDB(fieldsPrev, indicesPrev,columnToIndexPrev, db, getDefaultTableName(), conn);
                //??????????????????????????????????????????????????????????????????migrator
                if(fieldsPrev.size() == 0)
                    migrator.addCommand(Cmd("", Cmd::MIGACTION_CREATE_TABLE));
                    //??????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????model??????????????????
                else
                    migrator.addCommand(Cmd(getTableName(), Cmd::MIGACTION_CHANGE_TABLENAME, getDefaultTableName()));
            }
        }
        if(fieldsPrev.size() > 0)
        {
            if(tablenamePrev.length() != 0 && tablenamePrev != getTableName())
                migrator.addCommand(Cmd(getTableName(), Cmd::MIGACTION_CHANGE_TABLENAME, getDefaultTableName()));
            //????????????????????????
            for(FieldMeta& field: metadata)
            {
                //?????????????????????????????????
                if(columnToIndexPrev.find(field.getColumnName()) == columnToIndexPrev.end())
                {
                    //?????????????????????
                    auto it = fieldsPrev.cbegin();
                    for(; it != fieldsPrev.cend(); it++)
                        if(it->fieldName == field.fieldName)
                            break;
                    //?????????????????????????????????????????????
                    if(it == fieldsPrev.cend())
                    {
                        migrator.addCommand(Cmd(field.getColumnName(), Cmd::MIGACTION_ADD_COLUMN));
                        continue;
                    }
                        //??????????????????????????????????????????
                    else
                    {
                        migrator.addCommand(Cmd(field.getColumnName(), Cmd::MIGACTION_CHANGE_COLUMN,
                                                it->getColumnName()));
                        continue;
                    }
                }
                else
                {
                    if(field == fieldsPrev[columnToIndexPrev[field.getColumnName()]])
                        continue;
                    //?????????????????????????????????change action
                    migrator.addCommand(Cmd(field.getColumnName(), Cmd::MIGACTION_CHANGE_COLUMN));
                }
            }
            //??????????????????
            for(auto it = indicesPrev.begin(); it != indicesPrev.end(); it++)
            {
                //?????????????????????????????????drop index action
                if(indices.find(it->first) == indices.end())
                    migrator.addCommand(Cmd(it->first, Cmd::MIGACTION_DROP_INDEX));
                    //?????????????????????????????????????????????
                else if(!(it->second == indices.find(it->first)->second))
                {
                    migrator.addCommand(Cmd(it->first, Cmd::MIGACTION_DROP_INDEX));
                    migrator.addCommand(Cmd(it->first, Cmd::MIGACTION_ADD_INDEX));
                }
            }
            //??????????????????????????????
            for(auto it = indices.begin(); it != indices.end(); it++)
            {
                if(indicesPrev.find(it->first) == indicesPrev.end())
                    migrator.addCommand(Cmd(it->first, Cmd::MIGACTION_ADD_INDEX));
            }
        }
        //??????
        migrator.migrate();
        //????????????????????????????????????????????????saveModel?????????
        tablenamePrev = getTableName();
        metadata.swap(fieldsPrev);
        indices.swap(indicesPrev);
    }
}

#endif
