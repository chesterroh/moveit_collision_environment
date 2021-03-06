#include "collision_env.h"

std::vector<std::string> stringVectorSeparator (const std::string &input,
                                                const std::string &charToFind, const bool &debug)
{
    // This function will break the string to vector based on charToFind
    // Example: charToFind: $$, input string: random$$char$$_test_
    // The output of this function would be string vector with size 3 that contains: random, char, and _test_
    std::vector<std::string> result;
    std::size_t found = input.find(charToFind);
    std::size_t pos = 0;
    std::size_t index = 1;
    
    bool findSomething = (found!=std::string::npos);
    if (!findSomething) {
        return result;
    }
    
    while (found!=std::string::npos)
    {
        std::string buffer;
        buffer.assign(input, pos, found - pos);
        result.push_back(buffer);
        if (debug)
            std::cerr << "Found '" << charToFind << "' result: " << buffer << " " << index <<std::endl;
        pos = found + charToFind.length();
        index++;
        found = input.find(charToFind,pos);
    }
    
    std::string buffer;
    buffer.assign(input, pos, input.length() - pos);
    result.push_back(buffer);
    return result;
}

collision_environment::collision_environment()
{
    // Basic collision environment constructor. Set the class as not ready if there is no node handle assigned to it
    classReady = false; // not ready to be used
}

collision_environment::collision_environment(const ros::NodeHandle &nh)
{
    // Collision environment has node handle for reading params and TF
    this -> setNodeHandle(nh);
};

void collision_environment::setNodeHandle(const ros::NodeHandle &nh)
{
    // Set the nodehandle to collision_environment variable and read the params
    classReady = true;
    hasParent = false;
    hasTableTF = false;
    hasObjects = false;
    this->nh = nh;
    nh.param("objectNameIdx", objectNameFormatIndex,2);
    nh.param("debug", debug,false);
    nh.param("charToFind", charToFind,std::string("::"));
    nh.param("mesh_source", mesh_source,std::string("data/mesh"));
    nh.param("file_extension",file_extension,std::string("stl"));
    nh.param("tableTFname",tableTFname,std::string("tableTF"));
    nh.param("defineParent",defineParent,false);
    if (defineParent) {
        nh.param("parentFrameName",definedParent,std::string("base_link"));
    }
    
    this->segmentedObjects = new std::vector<moveit_msgs::CollisionObject>();
    
    std::cerr << "Started \n";
    
    // sleep for caching the initial TF frames.
    sleep(1.0);
    getAllObjectTF();
}

void collision_environment::updateCollisionObjects(const bool &updateFrame)
{
    // This function will update the collision object list based on the list of object TF
    if (!classReady)
    {
        std::cerr << "ERROR, class has no nodehandle to subscribe.\n";
        hasObjects = false;
        return;
    }
    
    // Remove all collision objects from cache
    segmentedObjects->clear();
    
    bool updateTable;
    nh.param("renewTable", updateTable, true);
    if (updateTable) {
        // update table and add it to list of collision objects
        if (getTable())
            segmentedObjects->push_back(this->tableObject);
    }
    else
        // use previous table data if available
        if (hasTableTF) {
            segmentedObjects->push_back(this->tableObject);
        }
    
    if (debug)
        std::cerr << "Number of object after add table: " << segmentedObjects->size() <<std::endl;

	// update list of object TF when updateCollisionObjects method is called
    if (updateFrame) {
        getAllObjectTF();
        if (listOfTF.size() == 0) {
            hasObjects = false;
            std::cerr << "No Object TF available.\n";
            return;
        }
    }
    else hasObjects = true;
    
    // Add the collision objects based on available object TF
    for (int i = 0; i < detectedObjectsTF.size(); i++) {
        moveit_msgs::CollisionObject co;
        // name of collision object = TF name
        co.id = detectedObjectsTF.at(i).frame_id;
        co.header.frame_id = parentFrame;
        std::stringstream ss;
        // read the location of the mesh file
        ss << "file://" << mesh_source << "/" << detectedObjectsTF.at(i).name << "." << file_extension;
        
        // Generate moveit mesh from mesh file
        shapes::Mesh * tmpMesh = shapes::createMeshFromResource(ss.str());
        shape_msgs::Mesh co_mesh;
        shapes::ShapeMsg co_mesh_msg;
        shapes::constructMsgFromShape(tmpMesh,co_mesh_msg);
        co_mesh = boost::get<shape_msgs::Mesh>(co_mesh_msg);
        co.meshes.push_back(co_mesh);
        co.mesh_poses.push_back(detectedObjectsTF.at(i).pose);
        co.operation = co.ADD;
        segmentedObjects->push_back(co);
    }
    
    if (debug) {
        std::cerr << "Number of object after add objects: ";
        std::cerr << segmentedObjects->size() <<std::endl;
        std::cerr << "Collision Object Published\n";
    }
};

void collision_environment::getAllObjectTF()
{
    // This function will read all TF frame ids, saves the TF frames that has object TF name signature, and get the parent frame of that object TF.
    
	// get the TF list
    listener.getFrameStrings(listOfTF);
    if (listOfTF.size() == 0) {
        std::cerr << "No Frame available yet.\n";
        return;
    }
    
    if (debug) {
        for (unsigned int i = 0; i < listOfTF.size(); i++) {
            std::cerr << "Frame " << i << ": " << listOfTF.at(i) <<std::endl;
        }
    }
    
    if (!hasParent) //searching for object tf parent
        for (unsigned int i = 0; i < listOfTF.size(); i++) {
            std::vector<std::string> tmp = stringVectorSeparator(listOfTF.at(i), charToFind, debug);
            if (tmp.size() > objectNameFormatIndex)
            {
                if (!defineParent) {
                    // read the TF parent of objectTF
                    listener.getParent(listOfTF.at(i),ros::Time(0),parentFrame);
                    if (debug)
                        std::cerr << "parentFrame of ObjectTF ->" << parentFrame << std::endl;
                }
                else
                    // use predefined parent name if we set param defineParent = true
                    parentFrame = definedParent;
                hasParent = true;
                break;
            }
        }
    
	detectedObjectsTF.clear();
    std::vector<std::string> newestListOfTF;
    
    ros::Time now = ros::Time::now();
    
    if (!hasParent) {
        // something wrong going on here, probably no object frame found or no new TF frame for this object
        std::cerr << "TF fail to get any Object frames\n";
        listOfTF.clear();
        return;
    }

    // get and save the TF name and pose in the class variable
    for (unsigned int i = 0; i < listOfTF.size(); i++) {
        std::vector<std::string> tmp = stringVectorSeparator(listOfTF.at(i), charToFind, debug);
        if (tmp.size() > objectNameFormatIndex)
        {
            if (listener.waitForTransform(listOfTF.at(i),parentFrame,now,ros::Duration(0.5)))
            {
                objectTF detectedObject;
                detectedObject.name = tmp.at(objectNameFormatIndex-1);
                detectedObject.frame_id = listOfTF.at(i);
                newestListOfTF.push_back(detectedObject.frame_id);
                tf::StampedTransform transform;
                listener.lookupTransform(parentFrame,listOfTF.at(i),ros::Time(0),transform);
                tf::poseTFToMsg(transform,detectedObject.pose);
                detectedObjectsTF.push_back(detectedObject);
            }
        }
	}
    
    // update the list of TF to contain only newest set of object TF frames
    listOfTF = newestListOfTF;
    
	// Print out the object tf list
    if (debug) {
        std::cerr <<"Num of Objects: " << detectedObjectsTF.size() <<std::endl;
        for (unsigned int i = 0; i < detectedObjectsTF.size(); i++)
            std::cerr << "name: " << detectedObjectsTF.at(i).name << ", frame id: " << detectedObjectsTF.at(i).frame_id << std::endl;
    }
};

const std::vector<moveit_msgs::CollisionObject> collision_environment::generateOldObjectToRemove() const
{
    // This function will generate list of collision objects generated from prior iteration
    std::vector<moveit_msgs::CollisionObject> objectsToRemove;
    for (unsigned int i = 0; i < segmentedObjects->size(); i++) {
        moveit_msgs::CollisionObject co;
        co.id = segmentedObjects->at(i).id;
        co.header.frame_id = segmentedObjects->at(i).header.frame_id;
        co.operation = co.REMOVE;
        objectsToRemove.push_back(co);
    }
    return objectsToRemove;
}

const std::vector<moveit_msgs::CollisionObject> collision_environment::getCollisionObjects() const
{
    return *segmentedObjects;
}

std::vector<std::string> collision_environment::getListOfTF() const
{
    return listOfTF;
}

bool collision_environment::getTable()
{
    // This function will read the table TF and generate box collision object with size 0.75x0.75x0.02 m located 2 cm below the table TF
    
    if (!classReady)
    {
        std::cerr << "ERROR, the collision environment class has no nodehandle to subscribe.\n";
        hasTableTF = false;
        return false;
    }
    
    moveit_msgs::CollisionObject co;
    std::string parentTableTF;
    
    if (!defineParent) {
        listener.getParent(tableTFname,ros::Time(0),parentTableTF);
    }
    else parentTableTF = definedParent;
    
    // try to get new table TF transform
    if (!listener.waitForTransform(tableTFname,parentTableTF,ros::Time::now(),ros::Duration(0.5)))
    {
        std::cerr << "Fail to get tf for table: " << tableTFname << std::endl;
        hasTableTF = false;
    }
    else
    {
        // Assigns the collision object parameters
        co.id = tableTFname;
        co.header.frame_id = parentTableTF;
        if (debug)
            std::cerr << "parentFrame of: " << tableTFname << "-> " << parentTableTF << std::endl;
        tf::StampedTransform transform;
        listener.lookupTransform(parentTableTF,tableTFname,ros::Time(0),transform);
        geometry_msgs::TransformStamped msg;
        transformStampedTFToMsg(transform, msg);
        shape_msgs::SolidPrimitive primitive;
        primitive.type = primitive.BOX;
        primitive.dimensions.resize(3);
        primitive.dimensions[0] = 0.75;
        primitive.dimensions[1] = 0.75;
        primitive.dimensions[2] = 0.02;
        
        geometry_msgs::Pose box_pose;
        tf::poseTFToMsg(transform,box_pose);
        box_pose.position.z -= primitive.dimensions[2];
        co.primitives.push_back(primitive);
        co.primitive_poses.push_back(box_pose);
        hasTableTF = true;
    }
    tableObject = co;
    return hasTableTF;
}
