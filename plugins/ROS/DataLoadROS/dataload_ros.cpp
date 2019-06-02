#include "dataload_ros.h"
#include <QTextStream>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <QApplication>
#include <QProgressDialog>
#include <QFileInfo>
#include <QProcess>
#include <rosbag/view.h>
#include <sys/sysinfo.h>
#include <QSettings>
#include <QElapsedTimer>

#include "../dialog_select_ros_topics.h"
#include "../shape_shifter_factory.hpp"
#include "../rule_editing.h"
#include "../dialog_with_itemlist.h"

DataLoadROS::DataLoadROS()
{
    _extensions.push_back( "bag");

    QSettings settings;

    _config.selected_topics      = settings.value("DataLoadROS/default_topics", false ).toStringList();
    _config.use_header_stamp     = settings.value("DataLoadROS/use_header_stamp", false ).toBool();
    _config.use_renaming_rules   = settings.value("DataLoadROS/use_renaming", true ).toBool();
    _config.max_array_size       = settings.value("DataLoadROS/max_array_size", 100 ).toInt();
    _config.discard_large_arrays = settings.value("DataLoadROS/discard_large_arrays", true ).toBool();
}

void StrCat(const std::string& a, const std::string& b,  std::string& out)
{
    out.clear();
    out.reserve(a.size() + b.size());
    out.assign(a);
    out.append(b);
}

const std::vector<const char*> &DataLoadROS::compatibleFileExtensions() const
{
    return _extensions;
}

size_t getAvailableRAM()
{
    struct sysinfo info;
    sysinfo(&info);
    return info.freeram;
}

std::vector<std::pair<QString,QString>> DataLoadROS::getAndRegisterAllTopics()
{
    std::vector<std::pair<QString,QString>> all_topics;
    rosbag::View bag_view ( *_bag, ros::TIME_MIN, ros::TIME_MAX, true );

    RosIntrospectionFactory::reset();

    for(auto& conn: bag_view.getConnections() )
    {
        const auto&  topic      =  conn->topic;
        const auto&  md5sum     =  conn->md5sum;
        const auto&  datatype   =  conn->datatype;
        const auto&  definition =  conn->msg_def;

        all_topics.push_back( std::make_pair(QString( topic.c_str()), QString( datatype.c_str()) ) );
        _ros_parser.registerSchema(
                    topic, md5sum, RosIntrospection::ROSType(datatype), definition);

        RosIntrospectionFactory::registerMessage(topic, md5sum, datatype, definition);
    }
    return all_topics;
}

void DataLoadROS::storeMessageInstancesAsUserDefined(PlotDataMapRef& plot_map)
{
    using namespace RosIntrospection;

    rosbag::View bag_view ( *_bag, ros::TIME_MIN, ros::TIME_MAX, false );

    RenamedValues renamed_value;

    std::string prefixed_name;

    PlotDataAny& plot_consecutive = plot_map.addUserDefined( "__consecutive_message_instances__" )->second;

    for(const rosbag::MessageInstance& msg_instance: bag_view )
    {
        const std::string& topic_name  = msg_instance.getTopic();
        double msg_time = msg_instance.getTime().toSec();
        auto data_point = PlotDataAny::Point(msg_time, nonstd::any(msg_instance) );
        plot_consecutive.pushBack( data_point );

        const std::string* key_ptr = &topic_name ;

        auto plot_pair = plot_map.user_defined.find( *key_ptr );

        if( plot_pair == plot_map.user_defined.end() )
        {
            plot_pair = plot_map.addUserDefined( *key_ptr );
        }
        PlotDataAny& plot_raw = plot_pair->second;
        plot_raw.pushBack( data_point );
    }
}


bool DataLoadROS::readDataFromFile(const FileLoadInfo& info, PlotDataMapRef& plot_map)
{
    if( _bag ) _bag->close();

    _bag = std::make_shared<rosbag::Bag>();
    _ros_parser.clear();

    try{
        _bag->open( info.filename.toStdString(), rosbag::bagmode::Read );
    }
    catch( rosbag::BagException&  ex)
    {
        QMessageBox::warning(nullptr, tr("Error"),
                             QString("rosbag::open thrown an exception:\n")+
                             QString(ex.what()) );
        return false;
    }

    auto all_topics = getAndRegisterAllTopics();

    //----------------------------------

    if( info.plugin_config.hasChildNodes() )
    {
        xmlLoadState( info.plugin_config.firstChildElement() );
    }
    else
    {
        DialogSelectRosTopics* dialog = new DialogSelectRosTopics( all_topics, _config );

        if( dialog->exec() == static_cast<int>(QDialog::Accepted) )
        {
            _config = dialog->getResult();
        }
        else{
            return false;
        }
    }

    QSettings settings;

    settings.setValue("DataLoadROS/default_topics", _config.selected_topics);
    settings.setValue("DataLoadROS/use_renaming", _config.use_renaming_rules);
    settings.setValue("DataLoadROS/use_header_stamp", _config.use_header_stamp);
    settings.setValue("DataLoadROS/max_array_size", (int)_config.max_array_size);
    settings.setValue("DataLoadROS/discard_large_arrays", _config.discard_large_arrays);

    _ros_parser.setUseHeaderStamp( _config.use_header_stamp );
    _ros_parser.setMaxArrayPolicy( _config.max_array_size, _config.discard_large_arrays );

    if( _config.use_renaming_rules )
    {
        _ros_parser.addRules( RuleEditing::getRenamingRules() );
    }

    //-----------------------------------
    std::set<std::string> topic_selected;
    for(const auto& topic: _config.selected_topics)
    {
        topic_selected.insert( topic.toStdString() );
    }

    QProgressDialog progress_dialog;
    progress_dialog.setLabelText("Loading... please wait");
    progress_dialog.setWindowModality( Qt::ApplicationModal );

    rosbag::View bag_view_selected ( true );
    bag_view_selected.addQuery( *_bag, [topic_selected](rosbag::ConnectionInfo const* connection)
    {
        return topic_selected.find( connection->topic ) != topic_selected.end();
    } );
    progress_dialog.setRange(0, bag_view_selected.size()-1);
    progress_dialog.show();

    std::vector<uint8_t> buffer;

    int msg_count = 0;

    QElapsedTimer timer;
    timer.start();

    for(const rosbag::MessageInstance& msg_instance: bag_view_selected )
    {
        const std::string& topic_name  = msg_instance.getTopic();
        const size_t msg_size  = msg_instance.size();

        buffer.resize(msg_size);

        if( msg_count++ %100 == 0)
        {
            progress_dialog.setValue( msg_count );
            QApplication::processEvents();

            if( progress_dialog.wasCanceled() ) {
                return false;
            }
        }

        ros::serialization::OStream stream(buffer.data(), buffer.size());
        msg_instance.write(stream);

        const double msg_time = msg_instance.getTime().toSec();

        MessageRef buffer_view( buffer );
        _ros_parser.pushMessageRef( topic_name, buffer_view, msg_time );
    }

    _ros_parser.extractData(plot_map, "");
    storeMessageInstancesAsUserDefined(plot_map);

    qDebug() << "The loading operation took" << timer.elapsed() << "milliseconds";

    return true;
}


DataLoadROS::~DataLoadROS()
{

}

QDomElement DataLoadROS::xmlSaveState(QDomDocument &doc) const
{
    QDomElement plugin_elem = doc.createElement("plugin");
    plugin_elem.setAttribute("ID", QString(this->name()).replace(" ", "_") );

    QString topics_list = _config.selected_topics.join(";");
    QDomElement list_elem = doc.createElement("selected_topics");
    list_elem.setAttribute("value", topics_list);
    plugin_elem.appendChild( list_elem );

    QDomElement stamp_elem = doc.createElement("use_header_stamp");
    stamp_elem.setAttribute("value", _config.use_header_stamp ? "true" : "false");
    plugin_elem.appendChild( stamp_elem );

    QDomElement rename_elem = doc.createElement("use_renaming_rules");
    rename_elem.setAttribute("value", _config.use_renaming_rules ? "true" : "false");
    plugin_elem.appendChild( rename_elem );

    QDomElement discard_elem = doc.createElement("discard_large_arrays");
    discard_elem.setAttribute("value", _config.discard_large_arrays ? "true" : "false");
    plugin_elem.appendChild( discard_elem );

    QDomElement max_elem = doc.createElement("max_array_size");
    max_elem.setAttribute("value", QString::number(_config.max_array_size));
    plugin_elem.appendChild( max_elem );

    return plugin_elem;
}

bool DataLoadROS::xmlLoadState(const QDomElement &parent_element)
{
    QDomElement list_elem = parent_element.firstChildElement( "selected_topics" );
    QString topics_list = list_elem.attribute("value");
    _config.selected_topics = topics_list.split(";", QString::SkipEmptyParts);

    QDomElement stamp_elem = parent_element.firstChildElement( "use_header_stamp" );
    _config.use_header_stamp = ( stamp_elem.attribute("value") == "true");

    QDomElement rename_elem = parent_element.firstChildElement( "use_renaming_rules" );
    _config.use_renaming_rules = ( rename_elem.attribute("value") == "true");

    QDomElement discard_elem = parent_element.firstChildElement( "discard_large_arrays" );
    _config.discard_large_arrays = ( discard_elem.attribute("value") == "true");

    QDomElement max_elem = parent_element.firstChildElement( "max_array_size" );
    _config.max_array_size = max_elem.attribute("value").toInt();

    return true;
}


