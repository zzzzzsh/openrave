// -*- coding: utf-8 -*-
// Copyright (C) 2006-2012 Rosen Diankov <rosen.diankov@gmail.com>
//
// This file is part of OpenRAVE.
// OpenRAVE is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "colladacommon.h"
using namespace ColladaDOM150;

#include <locale>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/time_facet.hpp>
#include <boost/algorithm/string.hpp>

#define LIBXML_SAX1_ENABLED
#include <libxml/globals.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h> // only for xmlNewInputFromFile()
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

/// \brief converts raw XML data to DAE using libxml2
namespace XMLtoDAE
{
struct XMLREADERDATA
{
    XMLREADERDATA(daeElementRef pelt, xmlParserCtxtPtr ctxt) : _ctxt(ctxt), _offset(0) {
        _elts.push_back(pelt);
    }
    xmlParserCtxtPtr _ctxt;
    list<daeElementRef> _elts;
    size_t _offset;
};

static void XMLErrorFunc(void *ctx, const char *msg, ...)
{
    va_list args;

    va_start(args, msg);
    RAVELOG_ERROR("XML Parse error: ");
    vprintf(msg,args);
    va_end(args);
}

static void DefaultStartElementSAXFunc(void *ctx, const xmlChar *name, const xmlChar **atts)
{
    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    daeElementRef pelt = pdata->_elts.back()->add((const char*)name);
    if( atts != NULL ) {
        for (int i = 0; (atts[i] != NULL); i+=2) {
            pelt->setAttribute((const char*)atts[i],(const char*)atts[i+1]);
        }
    }
    pdata->_elts.push_back(pelt);
}

static void DefaultEndElementSAXFunc(void *ctx, const xmlChar *name)
{
    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    pdata->_elts.pop_back();
    if( pdata->_elts.size() == 1 ) {
        BOOST_ASSERT(!!pdata->_ctxt->input);
        pdata->_offset = pdata->_ctxt->input->cur-pdata->_ctxt->input->base;
        xmlStopParser(pdata->_ctxt);
    }
}

static void DefaultCharactersSAXFunc(void *ctx, const xmlChar *ch, int len)
{
    XMLREADERDATA* pdata = (XMLREADERDATA*)ctx;
    pdata->_elts.back()->setCharData(string((const char*)ch, len));
}

static bool xmlDetectSAX2(xmlParserCtxtPtr ctxt)
{
    if (ctxt == NULL) {
        return false;
    }
#ifdef LIBXML_SAX1_ENABLED
    if ((ctxt->sax) &&  (ctxt->sax->initialized == XML_SAX2_MAGIC) && ((ctxt->sax->startElementNs != NULL) || (ctxt->sax->endElementNs != NULL))) {
        ctxt->sax2 = 1;
    }
#else
    ctxt->sax2 = 1;
#endif

    ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
    ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
    ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
    if ((ctxt->str_xml==NULL) || (ctxt->str_xmlns==NULL) || (ctxt->str_xml_ns == NULL)) {
        return false;
    }
    return true;
}

static size_t Parse(daeElementRef pelt, const char* buffer, int size)
{
    static xmlSAXHandler s_DefaultSAXHandler = { 0};
    if( size <= 0 ) {
        size = strlen(buffer);
        if( size == 0 ) {
            return 0;
        }
    }
    if( !s_DefaultSAXHandler.initialized ) {
        // first time, so init
        s_DefaultSAXHandler.startElement = DefaultStartElementSAXFunc;
        s_DefaultSAXHandler.endElement = DefaultEndElementSAXFunc;
        s_DefaultSAXHandler.characters = DefaultCharactersSAXFunc;
        s_DefaultSAXHandler.error = XMLErrorFunc;
        s_DefaultSAXHandler.initialized = 1;
    }

    xmlSAXHandlerPtr sax = &s_DefaultSAXHandler;
    int ret = 0;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (!ctxt ) {
        return 0;
    }
    if (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler) {
        xmlFree(ctxt->sax);
    }
    ctxt->sax = sax;
    xmlDetectSAX2(ctxt);

    XMLREADERDATA reader(pelt, ctxt);
    ctxt->userData = &reader;
    xmlParseDocument(ctxt);

    if (ctxt->wellFormed) {
        ret = 0;
    }
    else {
        if (ctxt->errNo != 0) {
            ret = ctxt->errNo;
        }
        else {
            ret = -1;
        }
    }
    if (sax != NULL) {
        ctxt->sax = NULL;
    }
    if (ctxt->myDoc != NULL) {
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    xmlFreeParserCtxt(ctxt);

    return ret==0 ? reader._offset : 0;
}
};

class ColladaWriter : public daeErrorHandler
{
public:
    struct SCENE
    {
        domVisual_sceneRef vscene;
        domKinematics_sceneRef kscene;
        domPhysics_sceneRef pscene;
        domInstance_with_extraRef viscene;
        domInstance_kinematics_sceneRef kiscene;
        domInstance_with_extraRef piscene;
    };

    struct LINKOUTPUT
    {
        list<pair<int,string> > listusedlinks;
        daeElementRef plink;
        domNodeRef pnode;
    };

    struct physics_model_output
    {
        domPhysics_modelRef pmodel;
        std::vector<std::string > vrigidbodysids;     ///< same ordering as the physics indices
    };

    struct kinematics_model_output
    {
        struct axis_output
        {
            //axis_output(const string& sid, KinBody::JointConstPtr pjoint, int iaxis) : sid(sid), pjoint(pjoint), iaxis(iaxis) {}
            axis_output() : iaxis(0) {
            }
            string sid;     // joint/axis
            string nodesid;
            KinBody::JointConstPtr pjoint;
            int iaxis;
            string jointnodesid;
        };
        KinBodyPtr pbody; ///< the body written for
        domKinematics_modelRef kmodel;
        domNodeRef noderoot; ///< root node containing the body transform it should have only one child pointing to the first link of the body
        std::vector<axis_output> vaxissids;     ///< no ordering
        std::vector<std::string > vlinksids;     ///< same ordering as the link indices
    };

    struct axis_sids
    {
        axis_sids() : value(0) {
        }
        axis_sids(const string& axissid, const string& valuesid, const string& jointnodesid) : axissid(axissid), valuesid(valuesid), jointnodesid(jointnodesid), value(0) {
        }
        string axissid, valuesid, jointnodesid;
        dReal value; // if valuesid is empty, use this float value
    };

    struct instance_kinematics_model_output
    {
        domInstance_kinematics_modelRef ikm;
        std::vector<axis_sids> vaxissids;
        boost::shared_ptr<kinematics_model_output> kmout;
        std::vector<std::pair<std::string,std::string> > vkinematicsbindings;     // node and kinematics model bindings
    };

    struct instance_physics_model_output
    {
        domInstance_physics_modelRef ipm;
        boost::shared_ptr<physics_model_output> pmout;
    };

    struct instance_articulated_system_output
    {
        KinBodyPtr pbody; ///< the body written for
        domInstance_articulated_systemRef ias;
        boost::shared_ptr<instance_physics_model_output> ipmout;
        std::vector<axis_sids> vaxissids;
        std::vector<std::string > vlinksids;
        std::vector<std::pair<std::string,std::string> > vkinematicsbindings;
    };

    struct kinbody_models
    {
        std::string uri, kinematicsgeometryhash;
        boost::shared_ptr<kinematics_model_output> kmout;
        boost::shared_ptr<physics_model_output> pmout;
    };


    class ColladaInterfaceWriter : public BaseXMLWriter
    {
public:
        ColladaInterfaceWriter(daeElementRef elt) : _elt(elt) {
        }
        const std::string& GetFormat() const {
            static const std::string _format("collada");
            return _format;
        }
        virtual BaseXMLWriterPtr AddChild(const std::string& xmltag, const AttributesList& atts) {
            daeElementRef childelt = _elt->add(xmltag.c_str());
            if( !childelt ) {
                throw OPENRAVE_EXCEPTION_FORMAT("collada writer failed to create child %s from parent %s",xmltag%_elt->getElementName(),ORE_InvalidArguments);
            }
            FOREACHC(itatt,atts) {
                if( !childelt->setAttribute(itatt->first.c_str(),itatt->second.c_str()) ) {
                    throw OPENRAVE_EXCEPTION_FORMAT("failed to add attribute %s to element %s", xmltag%itatt->first, ORE_InvalidArguments);
                }
            }
            return BaseXMLWriterPtr(new ColladaInterfaceWriter(childelt));
        }
        virtual void SetCharData(const std::string& data) {
            if( !_elt->setCharData(data) ) {
                throw OPENRAVE_EXCEPTION_FORMAT("failed to write char data to element %s: %s",_elt->getElementName()%data, ORE_InvalidState);
            }
        }

        virtual daeElementRef GetElement() const {
            return _elt;
        }
private:
        daeElementRef _elt;
    };

    ColladaWriter(EnvironmentBaseConstPtr penv, const AttributesList& atts) : _dom(NULL), _penv(penv)
    {
        _globalunit = 1.0;
        daeErrorHandler::setErrorHandler(this);
        RAVELOG_VERBOSE("init COLLADA writer version: %s, namespace: %s\n", COLLADA_VERSION, COLLADA_NAMESPACE);
        _dae.reset(new DAE);
        _bExternalRefAllBodies = false;
        _bForceWriteAll = false;
        _listExternalRefExports.clear();
        _listIgnoreExternalURIs.clear();
        FOREACHC(itatt,atts) {
            if( itatt->first == "externalref" ) {
                if( itatt->second == "*" ) {
                    _bExternalRefAllBodies = true;
                }
                else {
                    stringstream ss(itatt->second);
                    std::list<string> newelts((istream_iterator<string>(ss)), istream_iterator<string>());
                    _listExternalRefExports.splice(_listExternalRefExports.end(),newelts);
                }
            }
            else if( itatt->first == "ignoreexternaluri" ) {
                stringstream ss(itatt->second);
                std::list<string> newelts((istream_iterator<string>(ss)), istream_iterator<string>());
                _listIgnoreExternalURIs.splice(_listIgnoreExternalURIs.end(),newelts);
            }
            else if( itatt->first == "forcewirte" ) {
                if( itatt->second == "*" ) {
                    _bForceWriteAll = true;
                }
                else {
                    stringstream ss(itatt->second);
                    std::list<string> newelts((istream_iterator<string>(ss)), istream_iterator<string>());
                    _listForceWriteOptions.splice(_listForceWriteOptions.end(),newelts);
                }
            }
            else if( itatt->first == "skipwrite" ) {
                stringstream ss(itatt->second);
                std::list<string> newelts((istream_iterator<string>(ss)), istream_iterator<string>());
                _setSkipWriteOptions.insert(newelts.begin(),newelts.end());
            }
            else if( itatt->first == "openravescheme" ) {
                _vForceResolveOpenRAVEScheme = itatt->second;
            }
            else if( itatt->first == "unit" ) {
                _globalunit = boost::lexical_cast<dReal>(itatt->second);
            }
            else {
                if( !!_dae->getIOPlugin() ) {
                    // catch all
                    _dae->getIOPlugin()->setOption(itatt->first.c_str(),itatt->second.c_str());
                }
            }
        }
    }
    virtual ~ColladaWriter() {
    }

    /// \param docname the top level document?
    virtual void Init(const string& docname)
    {
        daeInt error = _dae->getDatabase()->insertDocument(docname.c_str(), &_doc );     // also creates a collada root
        BOOST_ASSERT( error == DAE_OK && !!_doc );
        _dom = daeSafeCast<domCOLLADA>(_doc->getDomRoot());

        //create the required asset tag
        domAssetRef asset = daeSafeCast<domAsset>( _dom->add( COLLADA_ELEMENT_ASSET ) );
        {
            // facet becomes owned by locale, so no need to explicitly delete
            boost::posix_time::time_facet* facet = new boost::posix_time::time_facet("%Y-%m-%dT%H:%M:%s");
            std::stringstream ss;
            ss.imbue(std::locale(ss.getloc(), facet));
            ss << boost::posix_time::second_clock::local_time();

            domAsset::domCreatedRef created = daeSafeCast<domAsset::domCreated>( asset->add( COLLADA_ELEMENT_CREATED ) );
            created->setValue(ss.str().c_str());
            domAsset::domModifiedRef modified = daeSafeCast<domAsset::domModified>( asset->add( COLLADA_ELEMENT_MODIFIED ) );
            modified->setValue(ss.str().c_str());

            domAsset::domContributorRef contrib = daeSafeCast<domAsset::domContributor>( asset->add( COLLADA_TYPE_CONTRIBUTOR ) );
            domAsset::domContributor::domAuthoring_toolRef authoringtool = daeSafeCast<domAsset::domContributor::domAuthoring_tool>( contrib->add( COLLADA_ELEMENT_AUTHORING_TOOL ) );
            authoringtool->setValue("OpenRAVE Collada Writer");

            domAsset::domUnitRef units = daeSafeCast<domAsset::domUnit>( asset->add( COLLADA_ELEMENT_UNIT ) );
            units->setMeter(1);
            units->setName("meter");

            domAsset::domUp_axisRef zup = daeSafeCast<domAsset::domUp_axis>( asset->add( COLLADA_ELEMENT_UP_AXIS ) );
            zup->setValue(UP_AXIS_Z_UP);
        }

        _globalscene = _dom->getScene();
        if( !_globalscene ) {
            _globalscene = daeSafeCast<domCOLLADA::domScene>( _dom->add( COLLADA_ELEMENT_SCENE ) );
        }

        if( IsWrite("visual") ) {
            _visualScenesLib = daeSafeCast<domLibrary_visual_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_VISUAL_SCENES));
            _visualScenesLib->setId("vscenes");

            if( IsWrite("geometry") ) {
                _geometriesLib = daeSafeCast<domLibrary_geometries>(_dom->add(COLLADA_ELEMENT_LIBRARY_GEOMETRIES));
                _geometriesLib->setId("geometries");
                _effectsLib = daeSafeCast<domLibrary_effects>(_dom->add(COLLADA_ELEMENT_LIBRARY_EFFECTS));
                _effectsLib->setId("effects");
                _materialsLib = daeSafeCast<domLibrary_materials>(_dom->add(COLLADA_ELEMENT_LIBRARY_MATERIALS));
                _materialsLib->setId("materials");
            }
        }

        _kinematicsModelsLib = daeSafeCast<domLibrary_kinematics_models>(_dom->add(COLLADA_ELEMENT_LIBRARY_KINEMATICS_MODELS));
        _kinematicsModelsLib->setId("kmodels");
        _articulatedSystemsLib = daeSafeCast<domLibrary_articulated_systems>(_dom->add(COLLADA_ELEMENT_LIBRARY_ARTICULATED_SYSTEMS));
        _articulatedSystemsLib->setId("asystems");
        _kinematicsScenesLib = daeSafeCast<domLibrary_kinematics_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_KINEMATICS_SCENES));
        _kinematicsScenesLib->setId("kscenes");
        if( IsWrite("physics") ) {
            _physicsScenesLib = daeSafeCast<domLibrary_physics_scenes>(_dom->add(COLLADA_ELEMENT_LIBRARY_PHYSICS_SCENES));
            _physicsScenesLib->setId("pscenes");
            _physicsModelsLib = daeSafeCast<domLibrary_physics_models>(_dom->add(COLLADA_ELEMENT_LIBRARY_PHYSICS_MODELS));
            _physicsModelsLib->setId("pmodels");
        }
        domExtraRef pextra_library_sensors = daeSafeCast<domExtra>(_dom->add(COLLADA_ELEMENT_EXTRA));
        pextra_library_sensors->setId("sensors");
        pextra_library_sensors->setType("library_sensors");
        _sensorsLib = daeSafeCast<domTechnique>(pextra_library_sensors->add(COLLADA_ELEMENT_TECHNIQUE));
        _sensorsLib->setProfile("OpenRAVE");
        domExtraRef pextra_library_actuators = daeSafeCast<domExtra>(_dom->add(COLLADA_ELEMENT_EXTRA));
        pextra_library_actuators->setId("actuators");
        pextra_library_actuators->setType("library_actuators");
        _actuatorsLib = daeSafeCast<domTechnique>(pextra_library_actuators->add(COLLADA_ELEMENT_TECHNIQUE));
        _actuatorsLib->setProfile("OpenRAVE");
    }

    virtual void Save(const string& filename)
    {
        if(!_dae->writeTo(_doc->getDocumentURI()->getURI(), filename.c_str()) ) {
            throw openrave_exception(str(boost::format("failed to save collada file to %s")%filename));
        }
    }

    /// \brief Write down environment
    virtual bool Write()
    {
        EnvironmentMutex::scoped_lock lockenv(_penv->GetMutex());
        vector<KinBodyPtr> vbodies;
        _penv->GetBodies(vbodies);
        std::list<KinBodyPtr> listbodies(vbodies.begin(),vbodies.end());
        return Write(listbodies);
    }

    virtual bool Write(const std::list<KinBodyPtr>& listbodies)
    {
        if( listbodies.size() == 0 ) {
            return false;
        }
        _CreateScene();
        domPhysics_scene::domTechnique_commonRef common = daeSafeCast<domPhysics_scene::domTechnique_common>(_scene.pscene->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));

        //  Create gravity
        domTargetable_float3Ref g = daeSafeCast<domTargetable_float3>(common->add(COLLADA_ELEMENT_GRAVITY));
        Vector vgravity = _penv->GetPhysicsEngine()->GetGravity();
        g->getValue().set3 (vgravity.x, vgravity.y, vgravity.z);

        std::list<boost::shared_ptr<instance_articulated_system_output> > listModelDatabase;
        FOREACHC(itbody,listbodies) {
            BOOST_ASSERT((*itbody)->GetEnv()==_penv);
            boost::shared_ptr<instance_articulated_system_output> iasout;
            if( _CheckForExternalWrite(*itbody) ) {
                iasout = _WriteKinBodyExternal(*itbody,_scene.kiscene);
            }
            else {
                iasout = _WriteKinBody(*itbody);
            }
            if( !!iasout ) {
                _WriteBindingsInstance_kinematics_scene(_scene.kiscene,KinBodyConstPtr(*itbody),iasout->vaxissids,iasout->vkinematicsbindings);
                listModelDatabase.push_back(iasout);
            }
            else {
                RAVELOG_WARN(str(boost::format("collada writer failed to write body %s\n")%(*itbody)->GetName()));
            }
        }
        _WriteDynamicRigidConstraints(_scene.piscene,listModelDatabase);
        return true;
    }

    /// \brief Write one robot as a file
    virtual bool Write(RobotBasePtr probot)
    {
        EnvironmentMutex::scoped_lock lockenv(_penv->GetMutex());
        _CreateScene();
        boost::shared_ptr<instance_articulated_system_output> iasout;
        if( _CheckForExternalWrite(probot) ) {
            iasout = _WriteKinBodyExternal(probot,_scene.kiscene);
        }
        else {
            iasout = _WriteKinBody(probot);
        }
        if( !iasout ) {
            return false;
        }
        _WriteBindingsInstance_kinematics_scene(_scene.kiscene,KinBodyConstPtr(probot),iasout->vaxissids,iasout->vkinematicsbindings);
        return true;
    }

    /// \brief Write one kinematic body as a file
    virtual bool Write(KinBodyPtr pbody)
    {
        if( pbody->IsRobot() ) {
            return Write(RaveInterfaceCast<RobotBase>(pbody));
        }
        EnvironmentMutex::scoped_lock lockenv(_penv->GetMutex());
        _CreateScene();
        boost::shared_ptr<instance_articulated_system_output> iasout;
        if( _CheckForExternalWrite(pbody) ) {
            iasout = _WriteKinBodyExternal(pbody,_scene.kiscene);
        }
        else {
            iasout = _WriteKinBody(pbody);
        }
        if( !iasout ) {
            return false;
        }
        _WriteBindingsInstance_kinematics_scene(_scene.kiscene,KinBodyConstPtr(pbody),iasout->vaxissids,iasout->vkinematicsbindings);
        return true;
    }

    /// \brief checks if a body can be written externally
    virtual bool _CheckForExternalWrite(KinBodyPtr pbody)
    {
        if( !_bExternalRefAllBodies && find(_listExternalRefExports.begin(),_listExternalRefExports.end(),pbody->GetName()) == _listExternalRefExports.end() ) {
            // user doesn't want to use external refs
            return false;
        }
        ColladaXMLReadablePtr pcolladainfo = boost::dynamic_pointer_cast<ColladaXMLReadable>(pbody->GetReadableInterface(ColladaXMLReadable::GetXMLIdStatic()));
        return !!pcolladainfo;
    }

    /// \brief compute correct external URI
    daeURI _ComputeExternalURI(const daeURI& uri)
    {
        // check if it comes from same document
        if( uri.getReferencedDocument() == _doc ) {
            return uri;
        }
        if( _vForceResolveOpenRAVEScheme.size() > 0 && uri.scheme() == "file" ) {
            // check if inside an openrave path, and if so, return the openrave relative directory instead using "openrave:"
            std::string filename;
            if( RaveInvertFileLookup(filename,uri.path()) ) {
                daeURI newuri(*_dae);
                newuri.set(_vForceResolveOpenRAVEScheme, "", string("/")+filename, uri.query(), uri.fragment());
                return newuri;
            }
        }
        return uri;
    }

    daeURI _ComputeBestURI(const std::list<std::string>& listURIs) {
        FOREACHC(ituri, listURIs) {
            daeURI uri(_ComputeExternalURI(daeURI(*_dae,*ituri)));
            if( _listIgnoreExternalURIs.size() == 0 ) {
                return uri;
            }
            string docuri = cdom::assembleUri(uri.scheme(), uri.authority(), uri.path(), "", "");
            if( find(_listIgnoreExternalURIs.begin(),_listIgnoreExternalURIs.end(),docuri) == _listIgnoreExternalURIs.end() ) {
                return uri;
            }
        }
        RAVELOG_WARN("failed to compute URI\n");
        return daeURI(*_dae);
    }

    /// \brief try to write kinbody as an external reference
    virtual boost::shared_ptr<instance_articulated_system_output> _WriteKinBodyExternal(KinBodyPtr pbody, domInstance_kinematics_sceneRef ikscene)
    {
        ColladaXMLReadablePtr pcolladainfo = boost::dynamic_pointer_cast<ColladaXMLReadable>(pbody->GetReadableInterface(ColladaXMLReadable::GetXMLIdStatic()));

        // save
        string asid = str(boost::format("body%d")%pbody->GetEnvironmentId());
        string askid = str(boost::format("%s_kinematics")%asid);
        string iassid = str(boost::format("%s_inst")%askid);
        domInstance_articulated_systemRef ias = daeSafeCast<domInstance_articulated_system>(_scene.kscene->add(COLLADA_ELEMENT_INSTANCE_ARTICULATED_SYSTEM));
        ias->setSid(iassid.c_str());
        ias->setUrl(_ComputeBestURI(pcolladainfo->_articulated_systemURIs));
        ias->setName(pbody->GetName().c_str());

        boost::shared_ptr<instance_articulated_system_output> iasout(new instance_articulated_system_output());
        iasout->pbody = pbody;
        iasout->ias = ias;

        if( (int)pcolladainfo->_bindingAxesSIDs.size() != pbody->GetDOF() ) {
            RAVELOG_WARN("_bindingAxesSIDs.size() != pbody->GetDOF()");
        }
        else {
            std::vector<dReal> vjointvalues;
            pbody->GetDOFValues(vjointvalues);
            iasout->vaxissids.resize(pcolladainfo->_bindingAxesSIDs.size());
            for(size_t idof = 0; idof < pcolladainfo->_bindingAxesSIDs.size(); ++idof) {
                // there's no way to directly call a setparam on a SIDREF, so have to <bind>
                std::string sparamref = str(boost::format("ias_param%d")%idof);
                domKinematics_newparamRef param = daeSafeCast<domKinematics_newparam>(ias->add(COLLADA_ELEMENT_NEWPARAM));
                param->setSid(sparamref.c_str());
                daeSafeCast<domKinematics_newparam::domSIDREF>(param->add(COLLADA_ELEMENT_SIDREF))->setValue(pcolladainfo->_bindingAxesSIDs[idof].kmodelaxissidref.c_str());
                iasout->vaxissids.at(idof).jointnodesid = str(boost::format("%s/%s")%_GetNodeId(pbody)%pcolladainfo->_bindingAxesSIDs[idof].nodesid);
                iasout->vaxissids.at(idof).axissid = sparamref;
                iasout->vaxissids.at(idof).value = vjointvalues.at(idof);
            }
        }

        Transform tnode = pbody->GetTransform();
        int imodel = 0;
        FOREACH(itmodel,pcolladainfo->_bindingModelURIs) {
            domNodeRef pnoderoot;
            string nodeid;
            if( IsWrite("visual") ) {
                //  Create root node for the visual scene
                pnoderoot = daeSafeCast<domNode>(_scene.vscene->add(COLLADA_ELEMENT_NODE));
                pnoderoot->setName(pbody->GetName().c_str());
                nodeid = _GetNodeId(pbody);
                pnoderoot->setId(nodeid.c_str());
                pnoderoot->setSid(nodeid.c_str());
                // write the body transform
                _WriteTransformation(pnoderoot, tnode, true);
                // write instance
                domInstance_nodeRef inode = daeSafeCast<domInstance_node>(pnoderoot->add(COLLADA_ELEMENT_INSTANCE_NODE));
                inode->setUrl(_ComputeExternalURI(daeURI(*_dae,itmodel->vmodel)));
                if( pbody->GetLinks().size() > 0 ) {
                    inode->setSid(_GetNodeSid(pbody->GetLinks().at(0)).c_str());
                }
                domExtraRef pinodeextra = daeSafeCast<domExtra>(inode->add(COLLADA_ELEMENT_EXTRA));
                pinodeextra->setType("idsuffix");
                pinodeextra->setName((string(".")+nodeid).c_str());

                // write bindings
                {
                    std::string smodelref = str(boost::format("ikmodel%d")%imodel);
                    domKinematics_newparamRef param = daeSafeCast<domKinematics_newparam>(ias->add(COLLADA_ELEMENT_NEWPARAM));
                    param->setSid(smodelref.c_str());
                    daeSafeCast<domKinematics_newparam::domSIDREF>(param->add(COLLADA_ELEMENT_SIDREF))->setValue(itmodel->ikmodelsidref.c_str());
                    iasout->vkinematicsbindings.push_back(make_pair(smodelref, str(boost::format("%s/%s")%nodeid%inode->getSid())));
                }
            }

            if( _bForceWriteAll || _listForceWriteOptions.size() > 0 ) {
                // have to write an articulated_system that contains the extra options
                RAVELOG_WARN("force writing options not supported yet");
            }

            if( IsWrite("physics") ) {
                boost::shared_ptr<instance_physics_model_output> ipmout(new instance_physics_model_output());
                ipmout->ipm = daeSafeCast<domInstance_physics_model>(_scene.pscene->add(COLLADA_ELEMENT_INSTANCE_PHYSICS_MODEL));

                if( IsWrite("visual") ) {
                    // because we're instantiating the node, the full url isn't needed
                    size_t fragmentindex = itmodel->vmodel.find_last_of('#');
                    if( fragmentindex != std::string::npos ) {
                        ipmout->ipm->setParent(_ComputeExternalURI(daeURI(*pnoderoot,itmodel->vmodel.substr(fragmentindex)+string(".")+nodeid)));
                    }
                    else {
                        ipmout->ipm->setParent(_ComputeExternalURI(daeURI(*pnoderoot,itmodel->vmodel)));
                    }
                }
                ipmout->ipm->setUrl(_ComputeExternalURI(daeURI(*ipmout->ipm,itmodel->pmodel)));
                ipmout->ipm->setSid(str(boost::format("pmodel%d_inst")%pbody->GetEnvironmentId()).c_str());

                // only write the links that are in this model
                FOREACH(itlink,pcolladainfo->_bindingLinkSIDs) {
                    if( itlink->index == imodel ) {
                        domInstance_rigid_bodyRef pirb = daeSafeCast<domInstance_rigid_body>(ipmout->ipm->add(COLLADA_ELEMENT_INSTANCE_RIGID_BODY));
                        pirb->setBody(itlink->pmodel.c_str());
                        pirb->setSid(itlink->pmodel.c_str());

                        if( IsWrite("visual") ) {
                            size_t fragmentindex = itlink->vmodel.find_last_of('#');
                            if( fragmentindex != std::string::npos ) {
                                pirb->setTarget(_ComputeExternalURI(daeURI(*pnoderoot,itlink->vmodel.substr(fragmentindex)+string(".")+nodeid)));
                            }
                            else {
                                pirb->setTarget(_ComputeExternalURI(daeURI(*pnoderoot,itlink->vmodel)));
                            }
                        }
                    }
                }
                iasout->ipmout = ipmout;
            }
            ++imodel;
        }

        return iasout;
    }

    void _WriteKinBodyType(KinBodyPtr pbody, daeElementRef eltbody)
    {
        // interface type
        domExtraRef pextra = daeSafeCast<domExtra>(eltbody->add(COLLADA_ELEMENT_EXTRA));
        pextra->setType("interface_type");
        domTechniqueRef ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
        ptec->setProfile("OpenRAVE");
        daeElementRef pelt = ptec->add("interface");
        pelt->setAttribute("type", pbody->IsRobot() ? "robot" : "kinbody");
        pelt->setCharData(pbody->GetXMLId());
    }

    /// \brief that is independent of the kinematics/visuals so should belong in the instance_* extra fields, preferably instance_articulated_system
    void _WriteKinBodyExtraInfo(KinBodyPtr pbody, daeElementRef eltbody)
    {
        if( IsWrite("readable") ) {
            BaseXMLWriterPtr extrawriter(new ColladaInterfaceWriter(eltbody));
            pbody->Serialize(extrawriter,0);
        }
    }

    /// \brief Write robot in a given scene
    virtual boost::shared_ptr<instance_articulated_system_output> _WriteKinBody(KinBodyPtr pbody)
    {
        RAVELOG_VERBOSE(str(boost::format("writing robot as instance_articulated_system (%d) %s\n")%pbody->GetEnvironmentId()%pbody->GetName()));
        string asid = str(boost::format("robot%d")%pbody->GetEnvironmentId());
        string askid = str(boost::format("%s_kinematics")%asid);
        string asmid = str(boost::format("%s_motion")%asid);
        string iassid = str(boost::format("%s_inst")%asmid);

        domInstance_articulated_systemRef ias = daeSafeCast<domInstance_articulated_system>(_scene.kscene->add(COLLADA_ELEMENT_INSTANCE_ARTICULATED_SYSTEM));
        ias->setSid(iassid.c_str());
        ias->setUrl((string("#")+asmid).c_str());
        ias->setName(pbody->GetName().c_str());

        boost::shared_ptr<instance_articulated_system_output> iasout(new instance_articulated_system_output());
        iasout->pbody = pbody;
        iasout->ias = ias;

        // motion info
        domArticulated_systemRef articulated_system_motion = daeSafeCast<domArticulated_system>(_articulatedSystemsLib->add(COLLADA_ELEMENT_ARTICULATED_SYSTEM));
        articulated_system_motion->setId(asmid.c_str());
        domMotionRef motion = daeSafeCast<domMotion>(articulated_system_motion->add(COLLADA_ELEMENT_MOTION));
        domMotion_techniqueRef mt = daeSafeCast<domMotion_technique>(motion->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
        domInstance_articulated_systemRef ias_motion = daeSafeCast<domInstance_articulated_system>(motion->add(COLLADA_ELEMENT_INSTANCE_ARTICULATED_SYSTEM));
        ias_motion->setUrl(str(boost::format("#%s")%askid).c_str());

        // kinematics info
        domArticulated_systemRef articulated_system_kinematics = daeSafeCast<domArticulated_system>(_articulatedSystemsLib->add(COLLADA_ELEMENT_ARTICULATED_SYSTEM));
        articulated_system_kinematics->setId(askid.c_str());
        domKinematicsRef kinematics = daeSafeCast<domKinematics>(articulated_system_kinematics->add(COLLADA_ELEMENT_KINEMATICS));
        domKinematics_techniqueRef kt = daeSafeCast<domKinematics_technique>(kinematics->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));

        boost::shared_ptr<instance_kinematics_model_output> ikmout = _WriteInstance_kinematics_model(pbody,kinematics,askid);

        std::string kmodelid = _GetKinematicsModelId(pbody);
        for(size_t idof = 0; idof < ikmout->vaxissids.size(); ++idof) {
            string kaxis_infosid = str(boost::format("kaxis_info_inst%d")%idof);
            string maxis_infosid = str(boost::format("maxis_info_inst%d")%idof);
            KinBody::JointConstPtr pjoint = ikmout->kmout->vaxissids.at(idof).pjoint;
            int iaxis = ikmout->kmout->vaxissids.at(idof).iaxis;

            //  Kinematics axis info
            domKinematics_axis_infoRef kai = daeSafeCast<domKinematics_axis_info>(kt->add(COLLADA_ELEMENT_AXIS_INFO));
            kai->setAxis(str(boost::format("%s/%s")%kmodelid%ikmout->kmout->vaxissids.at(idof).sid).c_str());
            kai->setSid(kaxis_infosid.c_str());

            // create a newparam for every element so that it could be overwritten in the future
            domKinematics_newparamRef param_active = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
            param_active->setSid("active");
            daeSafeCast<domKinematics_newparam::domBool>(param_active->add(COLLADA_ELEMENT_BOOL))->setValue(pjoint->GetDOFIndex()>=0);
            domCommon_bool_or_paramRef active = daeSafeCast<domCommon_bool_or_param>(kai->add(COLLADA_ELEMENT_ACTIVE));
            daeSafeCast<domCommon_param>(active->add(COLLADA_ELEMENT_PARAM))->setValue("active");

            domKinematics_newparamRef param_locked = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
            param_locked->setSid("locked");
            daeSafeCast<domKinematics_newparam::domBool>(param_locked->add(COLLADA_ELEMENT_BOOL))->setValue(false);
            domCommon_bool_or_paramRef locked = daeSafeCast<domCommon_bool_or_param>(kai->add(COLLADA_ELEMENT_LOCKED));
            daeSafeCast<domCommon_param>(locked->add(COLLADA_ELEMENT_PARAM))->setValue("locked");

            // write limits if not circular or not revolute
            if( !pjoint->IsCircular(iaxis) || !pjoint->IsRevolute(iaxis) ) {
                std::pair<dReal, dReal> jointaxislimit = pjoint->GetLimit(iaxis);
                dReal fmult = 1.0;
                if( pjoint->IsRevolute(iaxis) ) {
                    fmult = 180.0/PI;
                }
                domKinematics_newparamRef param_positionmin = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
                param_positionmin->setSid("positionmin");
                daeSafeCast<domKinematics_newparam::domFloat>(param_positionmin->add(COLLADA_ELEMENT_FLOAT))->setValue(jointaxislimit.first*fmult);
                domKinematics_newparamRef param_positionmax = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
                param_positionmax->setSid("positionmax");
                daeSafeCast<domKinematics_newparam::domFloat>(param_positionmax->add(COLLADA_ELEMENT_FLOAT))->setValue(jointaxislimit.first*fmult);

                domKinematics_limitsRef plimits = daeSafeCast<domKinematics_limits>(kai->add(COLLADA_ELEMENT_LIMITS));
                daeSafeCast<domCommon_param>(plimits->add(COLLADA_ELEMENT_MIN)->add(COLLADA_ELEMENT_PARAM))->setValue("positionmin");
                daeSafeCast<domCommon_param>(plimits->add(COLLADA_ELEMENT_MAX)->add(COLLADA_ELEMENT_PARAM))->setValue("positionmax");
            }
            domKinematics_newparamRef param_circular = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
            param_circular->setSid("circular");
            daeSafeCast<domKinematics_newparam::domBool>(param_circular->add(COLLADA_ELEMENT_BOOL))->setValue(pjoint->IsCircular(iaxis));
            domKinematics_newparamRef param_planning_weight = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
            param_planning_weight->setSid("planning_weight");
            daeSafeCast<domKinematics_newparam::domFloat>(param_planning_weight->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->GetWeight(iaxis));
            domKinematics_newparamRef param_discretization_resolution = daeSafeCast<domKinematics_newparam>(kai->add(COLLADA_ELEMENT_NEWPARAM));
            param_discretization_resolution->setSid("discretization_resolution");
            daeSafeCast<domKinematics_newparam::domFloat>(param_discretization_resolution->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->GetResolution(iaxis));

            //  Motion axis info
            domMotion_axis_infoRef mai = daeSafeCast<domMotion_axis_info>(mt->add(COLLADA_ELEMENT_AXIS_INFO));
            mai->setSid(maxis_infosid.c_str());
            mai->setAxis(str(boost::format("%s/%s")%askid%kaxis_infosid).c_str());
            domKinematics_newparamRef param_speed = daeSafeCast<domKinematics_newparam>(mai->add(COLLADA_ELEMENT_NEWPARAM));
            param_speed->setSid("speed");
            daeSafeCast<domKinematics_newparam::domFloat>(param_speed->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->GetMaxVel(iaxis));
            domCommon_float_or_paramRef speed = daeSafeCast<domCommon_float_or_param>(mai->add(COLLADA_ELEMENT_SPEED));
            daeSafeCast<domCommon_param>(speed->add(COLLADA_ELEMENT_PARAM))->setValue("speed");

            domKinematics_newparamRef param_acceleration = daeSafeCast<domKinematics_newparam>(mai->add(COLLADA_ELEMENT_NEWPARAM));
            param_acceleration->setSid("acceleration");
            daeSafeCast<domKinematics_newparam::domFloat>(param_acceleration->add(COLLADA_ELEMENT_FLOAT))->setValue(pjoint->GetMaxAccel(iaxis));
            domCommon_float_or_paramRef acceleration = daeSafeCast<domCommon_float_or_param>(mai->add(COLLADA_ELEMENT_ACCELERATION));
            daeSafeCast<domCommon_param>(acceleration->add(COLLADA_ELEMENT_PARAM))->setValue("acceleration");
        }

        // write the bindings
        string asmsym = str(boost::format("%s_%s")%asmid%ikmout->ikm->getSid());
        string assym = str(boost::format("%s_%s")%_scene.kscene->getID()%ikmout->ikm->getSid());
        FOREACH(it, ikmout->vkinematicsbindings) {
            domKinematics_newparamRef abm = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
            abm->setSid(asmsym.c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(abm->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%it->first).c_str());
            domKinematics_newparamRef ab = daeSafeCast<domKinematics_newparam>(ias->add(COLLADA_ELEMENT_NEWPARAM));
            ab->setSid(assym.c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(ab->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%asmid%asmsym).c_str());
            iasout->vkinematicsbindings.push_back(make_pair(string(ab->getSid()), it->second));
        }
        for(size_t idof = 0; idof < ikmout->vaxissids.size(); ++idof) {
            const axis_sids& kas = ikmout->vaxissids.at(idof);
            domKinematics_newparamRef abm = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
            abm->setSid(str(boost::format("%s_%s")%asmid%kas.axissid).c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(abm->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%kas.axissid).c_str());
            domKinematics_newparamRef ab = daeSafeCast<domKinematics_newparam>(ias->add(COLLADA_ELEMENT_NEWPARAM));
            ab->setSid(str(boost::format("%s_%s")%assym%kas.axissid).c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(ab->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s_%s")%asmid%asmid%kas.axissid).c_str());
            string valuesid;
            if( kas.valuesid.size() > 0 ) {
                domKinematics_newparamRef abmvalue = daeSafeCast<domKinematics_newparam>(ias_motion->add(COLLADA_ELEMENT_NEWPARAM));
                abmvalue->setSid(str(boost::format("%s_%s")%asmid%kas.valuesid).c_str());
                daeSafeCast<domKinematics_newparam::domSIDREF>(abmvalue->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s")%askid%kas.valuesid).c_str());
                domKinematics_newparamRef abvalue = daeSafeCast<domKinematics_newparam>(ias->add(COLLADA_ELEMENT_NEWPARAM));
                valuesid = str(boost::format("%s_%s")%assym%kas.valuesid);
                abvalue->setSid(valuesid.c_str());
                daeSafeCast<domKinematics_newparam::domSIDREF>(abvalue->add(COLLADA_ELEMENT_SIDREF))->setValue(str(boost::format("%s/%s_%s")%asmid%asmid%kas.valuesid).c_str());
            }
            iasout->vaxissids.push_back(axis_sids(string(ab->getSid()),valuesid,kas.jointnodesid));
        }

        if( IsWrite("physics") ) {
            iasout->ipmout = _WriteInstance_physics_model(pbody,_scene.pscene,_scene.pscene->getID());
        }
        _WriteKinBodyExtraInfo(pbody,articulated_system_motion);
        _WriteKinBodyType(pbody,articulated_system_motion);

        boost::shared_ptr<kinematics_model_output> kmout = _GetKinematics_model(pbody);
        kmodelid += "/";
        FOREACHC(itjoint,pbody->GetJoints()) {
            domExtraRef pextra = daeSafeCast<domExtra>(articulated_system_motion->add(COLLADA_ELEMENT_EXTRA));
            pextra->setName(str(boost::format("motor%d")%(*itjoint)->GetJointIndex()).c_str());
            pextra->setType("attach_actuator");
            domTechniqueRef ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
            ptec->setProfile("OpenRAVE");
            daeElementRef bind_actuator = ptec->add("bind_actuator");
            bind_actuator->setAttribute("joint",str(boost::format("%sjoint%d")%kmodelid%(*itjoint)->GetJointIndex()).c_str());
        }

        if( pbody->IsRobot() ) {
            RobotBasePtr probot = RaveInterfaceCast<RobotBase>(pbody);
            if( IsWrite("manipulators") ) {
                FOREACHC(itmanip, probot->GetManipulators()) {
                    domExtraRef pextra = daeSafeCast<domExtra>(articulated_system_motion->add(COLLADA_ELEMENT_EXTRA));
                    pextra->setName((*itmanip)->GetName().c_str());
                    pextra->setType("manipulator");
                    domTechniqueRef ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
                    ptec->setProfile("OpenRAVE");
                    daeElementRef frame_origin = ptec->add("frame_origin");
                    frame_origin->setAttribute("link",(kmodelid+kmout->vlinksids.at((*itmanip)->GetBase()->GetIndex())).c_str());
                    daeElementRef frame_tip = ptec->add("frame_tip");
                    frame_tip->setAttribute("link",(kmodelid+kmout->vlinksids.at((*itmanip)->GetEndEffector()->GetIndex())).c_str());
                    _WriteTransformation(frame_tip,(*itmanip)->GetLocalToolTransform());
                    {
                        daeElementRef direction = frame_tip->add("direction");
                        stringstream ss; ss << std::setprecision(std::numeric_limits<OpenRAVE::dReal>::digits10+1);
                        ss << (*itmanip)->GetLocalToolDirection().x << " " << (*itmanip)->GetLocalToolDirection().y << " " << (*itmanip)->GetLocalToolDirection().z;
                        direction->setCharData(ss.str());
                    }
                    int i = 0;
                    map<KinBody::JointPtr, daeElementRef> mapgripper_joints;
                    FOREACHC(itindex,(*itmanip)->GetGripperIndices()) {
                        KinBody::JointPtr pjoint = probot->GetJointFromDOFIndex(*itindex);
                        BOOST_ASSERT(pjoint->GetJointIndex()>=0);
                        daeElementRef gripper_joint;
                        if( mapgripper_joints.find(pjoint) == mapgripper_joints.end() ) {
                            gripper_joint = ptec->add("gripper_joint");
                            gripper_joint->setAttribute("joint",str(boost::format("%sjoint%d")%kmodelid%pjoint->GetJointIndex()).c_str());
                        }
                        else {
                            gripper_joint = mapgripper_joints[pjoint];
                        }
                        daeElementRef closing_direction = gripper_joint->add("closing_direction");
                        closing_direction->setAttribute("axis",str(boost::format("./axis%d")%(*itindex-pjoint->GetDOFIndex())).c_str());
                        closing_direction->add("float")->setCharData(boost::lexical_cast<std::string>((*itmanip)->GetClosingDirection().at(i)));
                        ++i;
                    }
                    //            <iksolver interface="WAM7ikfast" type="Transform6D">
                    //              <free_axis axis="jointname3"/>
                    //            </iksolver>
                    //            <iksolver type="Translation3D">
                    //              <free_axis axis="jointname4"/>
                    //            </iksolver>
                }
            }

            if( IsWrite("sensors") ) {
                //            if (probot->GetAttachedSensors().size() > 0)
                //            {
                //                domExtraRef extra   =   daeSafeCast<domExtra>(askinematics->add(COLLADA_ELEMENT_EXTRA));
                //                extra->setType("sensors");
                //                domTechniqueRef tech    =   daeSafeCast<domTechnique>(extra->add(COLLADA_ELEMENT_TECHNIQUE));
                //                tech->setProfile("OpenRAVE");
                //
                //                    for (size_t i = 0; i < probot->GetAttachedSensors().size();i++)
                //                    {
                //                        string  strsensor   =   string("sensor")+toString(i)+string("_")+probot->GetName();
                //                        string  strurl      =   string("#") + strsensor;
                //                        RobotBase::AttachedSensorPtr  asensor = probot->GetAttachedSensors().at(i);
                //
                //                        //  Instance of sensor into 'articulated_system'
                //                        domInstance_sensorRef   isensor =   daeSafeCast<domInstance_sensor>(tech->add(COLLADA_ELEMENT_INSTANCE_SENSOR));
                //                        isensor->setId(asensor->GetName().c_str());
                //                        isensor->setLink(asensor->GetAttachingLink()->GetName().c_str());
                //                        isensor->setUrl(strurl.c_str());
                //
                //                        //  Sensor definition into 'library_sensors'
                //                        domSensorRef    sensor  =   daeSafeCast<domSensor>(_sensorsLib->add(COLLADA_ELEMENT_SENSOR));
                //                        sensor->setType(asensor->GetSensor()->GetXMLId().c_str());
                //                        sensor->setId(strsensor.c_str());
                //                        sensor->setName(strsensor.c_str());
                //
                //                        RAVELOG_VERBOSE("Plugin Name: %s\n",asensor->GetSensor()->GetXMLId().c_str());
                //                    }
                //            }
            }
        }

        return iasout;
    }

    /// \brief Write common kinematic body in a given scene, called by _WriteKinBody
    virtual boost::shared_ptr<instance_kinematics_model_output> _WriteInstance_kinematics_model(KinBodyPtr pbody, daeElementRef parent, const string& sidscope)
    {
        EnvironmentMutex::scoped_lock lockenv(_penv->GetMutex());
        RAVELOG_VERBOSE(str(boost::format("writing instance_kinematics_model (%d) %s\n")%pbody->GetEnvironmentId()%pbody->GetName()));
        boost::shared_ptr<kinematics_model_output> kmout = WriteKinematics_model(pbody);

        boost::shared_ptr<instance_kinematics_model_output> ikmout(new instance_kinematics_model_output());
        ikmout->kmout = kmout;
        ikmout->ikm = daeSafeCast<domInstance_kinematics_model>(parent->add(COLLADA_ELEMENT_INSTANCE_KINEMATICS_MODEL));

        string symscope, refscope;
        if( sidscope.size() > 0 ) {
            symscope = sidscope+string("_");
            refscope = sidscope+string("/");
        }
        string kmodelid = _GetKinematicsModelId(pbody);
        string ikmsid = str(boost::format("%s_inst")%kmodelid);
        ikmout->ikm->setUrl(str(boost::format("#%s")%kmout->kmodel->getID()).c_str());
        ikmout->ikm->setSid(ikmsid.c_str());

        if( IsWrite("visual") ) {
            domKinematics_newparamRef kbind = daeSafeCast<domKinematics_newparam>(ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
            kbind->setSid((symscope+ikmsid).c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(kbind->add(COLLADA_ELEMENT_SIDREF))->setValue((refscope+ikmsid).c_str());
            // needs to be node0 instead of _GetNodeId(pbody) since the kinematics hierarchy origin does not have the current body's transform
            ikmout->vkinematicsbindings.push_back(make_pair(string(kbind->getSid()), str(boost::format("%s/node0")%_GetNodeId(pbody))));
        }

        ikmout->vaxissids.reserve(kmout->vaxissids.size());
        int i = 0;
        FOREACH(it,kmout->vaxissids) {
            domKinematics_newparamRef kbind = daeSafeCast<domKinematics_newparam>(ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
            string ref = it->sid;
            size_t index = ref.find("/");
            while(index != string::npos) {
                ref[index] = '.';
                index = ref.find("/",index+1);
            }
            string sid = symscope+ikmsid+"_"+ref;
            kbind->setSid(sid.c_str());
            daeSafeCast<domKinematics_newparam::domSIDREF>(kbind->add(COLLADA_ELEMENT_SIDREF))->setValue((refscope+ikmsid+"/"+it->sid).c_str());
            domKinematics_newparamRef pvalueparam = daeSafeCast<domKinematics_newparam>(ikmout->ikm->add(COLLADA_ELEMENT_NEWPARAM));
            pvalueparam->setSid((sid+string("_value")).c_str());
            dReal value = it->pjoint->GetValue(it->iaxis);
            daeSafeCast<domKinematics_newparam::domFloat>(pvalueparam->add(COLLADA_ELEMENT_FLOAT))->setValue(value);
            ikmout->vaxissids.push_back(axis_sids(sid,pvalueparam->getSid(),kmout->vaxissids.at(i).jointnodesid));
            ++i;
        }

        return ikmout;
    }

    virtual boost::shared_ptr<instance_physics_model_output> _WriteInstance_physics_model(KinBodyPtr pbody, daeElementRef parent, const string& sidscope)
    {
        if( !IsWrite("physics") ) {
            return boost::shared_ptr<instance_physics_model_output>();
        }
        boost::shared_ptr<physics_model_output> pmout = WritePhysics_model(pbody);
        boost::shared_ptr<instance_physics_model_output> ipmout(new instance_physics_model_output());
        ipmout->pmout = pmout;
        ipmout->ipm = daeSafeCast<domInstance_physics_model>(parent->add(COLLADA_ELEMENT_INSTANCE_PHYSICS_MODEL));
        string nodeid = _GetNodeId(pbody);
        boost::shared_ptr<kinematics_model_output> kmout = _GetKinematics_model(pbody);
        if( !kmout ) {
            RAVELOG_WARN(str(boost::format("kinematics_model for %s should be present")%pbody->GetName()));
        }

        //ipmout->ipm->setParent(daeURI(*ipmout->ipm,string("#")+_GetNodeId(pbody->GetLinks().at(0))));
        string symscope, refscope;
        if( sidscope.size() > 0 ) {
            symscope = sidscope+string("_");
            refscope = sidscope+string("/");
        }
        string ipmsid = str(boost::format("%s_inst")%pmout->pmodel->getID());
        ipmout->ipm->setUrl(str(boost::format("#%s")%pmout->pmodel->getID()).c_str());
        ipmout->ipm->setSid(ipmsid.c_str());
        for(size_t i = 0; i < pmout->vrigidbodysids.size(); ++i) {
            domInstance_rigid_bodyRef pirb = daeSafeCast<domInstance_rigid_body>(ipmout->ipm->add(COLLADA_ELEMENT_INSTANCE_RIGID_BODY));
            pirb->setBody(pmout->vrigidbodysids[i].c_str());
            // On export you can create the URI with the string, ie daeURI uri( body1->getSid() );
            pirb->setSid(pmout->vrigidbodysids[i].c_str()); // set the same sid as <rigid_body> sid just in case
            string rigidnodeid="#";
            if( !kmout ) {
                rigidnodeid += nodeid;
            }
            else {
                if( kmout->pbody == pbody ) {
                    rigidnodeid += _GetNodeId(KinBody::LinkConstPtr(pbody->GetLinks().at(i)));
                }
                else {
                    // todo, how do we assign ids to instanced nodes?
                    // current method is probably unsupported
                    rigidnodeid += str(boost::format("%s.%s")%_GetNodeId(KinBody::LinkConstPtr(kmout->pbody->GetLinks().at(i)))%nodeid);
                }
            }
            pirb->setTarget(daeURI(*pirb,rigidnodeid));

            if( i == 0 ) {
                // always use the node pointing to the first link since that doesn't have any transforms applied and all of the physics model's rigid bodies are in the first link's local coordinate sysytem.
                ipmout->ipm->setParent(daeURI(*ipmout->ipm,rigidnodeid));
            }
        }

        return ipmout;
    }

    virtual boost::shared_ptr<kinematics_model_output> WriteKinematics_model(KinBodyPtr pbody)
    {
        EnvironmentMutex::scoped_lock lockenv(_penv->GetMutex());
        boost::shared_ptr<kinematics_model_output> kmout = _GetKinematics_model(pbody);
        if( !!kmout ) {
            if( IsWrite("visual") ) {
                // the base model is the same, but the instance information like joint values and visual transform could be different
                vector<dReal> vjointvalues;
                pbody->GetDOFValues(vjointvalues);
                Transform tnode = pbody->GetTransform();

                //  Create root node for the visual scene
                domNodeRef pnoderoot = daeSafeCast<domNode>(_scene.vscene->add(COLLADA_ELEMENT_NODE));
                string bodyid = _GetNodeId(KinBodyConstPtr(pbody));
                pnoderoot->setId(bodyid.c_str());
                pnoderoot->setSid(bodyid.c_str());
                pnoderoot->setName(pbody->GetName().c_str());
                // write the body transform
                _WriteTransformation(pnoderoot, tnode);

                // create an instance_node pointing to kmout
                domInstance_nodeRef inode = daeSafeCast<domInstance_node>(pnoderoot->add(COLLADA_ELEMENT_INSTANCE_NODE));
                domNodeRef refnodelink = daeSafeCast<domNode>(kmout->noderoot->getChild("node"));
                OPENRAVE_ASSERT_FORMAT(!!refnodelink,"node root %s should have at least one child",kmout->noderoot->getName(),ORE_Assert);
                inode->setUrl(str(boost::format("#%s")%refnodelink->getId()).c_str());
                if( pbody->GetLinks().size() > 0 ) {
                    inode->setSid(_GetNodeSid(pbody->GetLinks().at(0)).c_str());
                    domExtraRef pinodeextra = daeSafeCast<domExtra>(inode->add(COLLADA_ELEMENT_EXTRA));
                    pinodeextra->setType("idsuffix");
                    pinodeextra->setName((string(".")+bodyid).c_str());
                }
            }
            return kmout;
        }

        domKinematics_modelRef kmodel = daeSafeCast<domKinematics_model>(_kinematicsModelsLib->add(COLLADA_ELEMENT_KINEMATICS_MODEL));
        string kmodelid = _GetKinematicsModelId(pbody);
        kmodel->setId(kmodelid.c_str());
        kmodel->setName(pbody->GetName().c_str());

        // add description
        {
            domAssetRef kmodelinfo = daeSafeCast<domAsset>(kmodel->add(COLLADA_ELEMENT_ASSET));
            domAsset::domSubjectRef subject = daeSafeCast<domAsset::domSubject>(kmodelinfo->add(COLLADA_ELEMENT_SUBJECT));
            subject->setValue(pbody->GetDescription().c_str());
        }

        //kmodel->getAsset();
        domKinematics_model_techniqueRef ktec = daeSafeCast<domKinematics_model_technique>(kmodel->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));

        kmout.reset(new kinematics_model_output());

        KinBody::KinBodyStateSaver saver(pbody);
        vector<dReal> vjointvalues, vzero(pbody->GetDOF());
        pbody->GetDOFValues(vjointvalues);
        pbody->SetDOFValues(vzero);
        Transform tnode = pbody->GetTransform();
        pbody->SetTransform(Transform());
        string bodyid = _GetNodeId(KinBodyConstPtr(pbody));
        domNodeRef pnoderoot;
        if( IsWrite("visual") ) {
            //  Create root node for the visual scene
            pnoderoot = daeSafeCast<domNode>(_scene.vscene->add(COLLADA_ELEMENT_NODE));
            pnoderoot->setId(bodyid.c_str());
            pnoderoot->setSid(bodyid.c_str());
            pnoderoot->setName(pbody->GetName().c_str());
            // write the body transform before the link nodes start to make it possible
            // to reuse the visual scene for other bodies
            _WriteTransformation(pnoderoot, tnode);
            kmout->noderoot = pnoderoot;
        }

        //  Declare all the joints
        vector< pair<int,KinBody::JointConstPtr> > vjoints;
        vjoints.reserve(pbody->GetJoints().size()+pbody->_vPassiveJoints.size());
        FOREACHC(itj, pbody->GetJoints() ) {
            vjoints.push_back(make_pair((*itj)->GetJointIndex(),*itj));
        }
        int index=pbody->GetJoints().size();
        FOREACHC(itj, pbody->_vPassiveJoints) {
            vjoints.push_back(make_pair(index++,*itj));
        }
        vector<dReal> lmin, lmax;
        vector<domJointRef> vdomjoints(vjoints.size());
        kmout->pbody = pbody;
        kmout->kmodel = kmodel;
        kmout->vaxissids.resize(0);
        kmout->vlinksids.resize(pbody->GetLinks().size());

        FOREACHC(itjoint, vjoints) {
            KinBody::JointConstPtr pjoint = itjoint->second;
            if(( pjoint->GetType() == KinBody::Joint::JointUniversal) ||( pjoint->GetType() == KinBody::Joint::JointHinge2) ||( pjoint->GetType() == KinBody::Joint::JointSpherical) ) {
                RAVELOG_WARN(str(boost::format("unsupported joint type specified 0x%x\n")%pjoint->GetType()));
                continue;
            }

            domJointRef pdomjoint = daeSafeCast<domJoint>(ktec->add(COLLADA_ELEMENT_JOINT));
            string jointsid = str(boost::format("joint%d")%itjoint->first);
            pdomjoint->setSid( jointsid.c_str() );
            pdomjoint->setName(pjoint->GetName().c_str());
            pjoint->GetLimits(lmin, lmax);
            vector<domAxis_constraintRef> vaxes(pjoint->GetDOF());
            for(int ia = 0; ia < pjoint->GetDOF(); ++ia) {
                dReal fmult = 1.0;
                if( pjoint->IsRevolute(ia) ) {
                    fmult = 180.0f/PI;
                    vaxes[ia] = daeSafeCast<domAxis_constraint>(pdomjoint->add(COLLADA_ELEMENT_REVOLUTE));
                }
                else {
                    vaxes[ia] = daeSafeCast<domAxis_constraint>(pdomjoint->add(COLLADA_ELEMENT_PRISMATIC));
                }
                string axisid = str(boost::format("axis%d")%ia);
                vaxes[ia]->setSid(axisid.c_str());
                kinematics_model_output::axis_output axissid;
                axissid.pjoint = pjoint;
                axissid.sid = jointsid+string("/")+axisid;
                axissid.iaxis = ia;
                axissid.jointnodesid = str(boost::format("%s/%s")%bodyid%_GetJointNodeSid(pjoint,ia));
                kmout->vaxissids.push_back(axissid);
                domAxisRef paxis = daeSafeCast<domAxis>(vaxes.at(ia)->add(COLLADA_ELEMENT_AXIS));
                paxis->getValue().setCount(3);
                paxis->getValue()[0] = pjoint->GetInternalHierarchyAxis(ia).x;
                paxis->getValue()[1] = pjoint->GetInternalHierarchyAxis(ia).y;
                paxis->getValue()[2] = pjoint->GetInternalHierarchyAxis(ia).z;
                if( !pjoint->IsCircular(ia) ) {
                    domJoint_limitsRef plimits = daeSafeCast<domJoint_limits>(vaxes[ia]->add(COLLADA_TYPE_LIMITS));
                    daeSafeCast<domMinmax>(plimits->add(COLLADA_ELEMENT_MIN))->getValue() = lmin.at(ia)*fmult;
                    daeSafeCast<domMinmax>(plimits->add(COLLADA_ELEMENT_MAX))->getValue() = lmax.at(ia)*fmult;
                }
            }
            vdomjoints.at(itjoint->first) = pdomjoint;
        }

        list<int> listunusedlinks;
        FOREACHC(itlink,pbody->GetLinks()) {
            listunusedlinks.push_back((*itlink)->GetIndex());
        }

        domNodeRef nodehead = pnoderoot;
        while(listunusedlinks.size()>0) {
            LINKOUTPUT childinfo = _WriteLink(pbody->GetLinks().at(listunusedlinks.front()), ktec, nodehead, kmodel->getID(), vjoints);
            Transform t = pbody->GetLinks()[listunusedlinks.front()]->GetTransform();
            _WriteTransformation(childinfo.plink, t);
            if( IsWrite("visual") ) {
                _WriteTransformation(childinfo.pnode, t);
            }
            FOREACHC(itused, childinfo.listusedlinks) {
                kmout->vlinksids.at(itused->first) = itused->second;
                listunusedlinks.remove(itused->first);
            }
            // update the root so that newer nodes go inside the hierarchy of the first link
            // this is necessary for instance_node to work correctly and to get the relative transform of the link right
            nodehead = childinfo.pnode;
        }

        _WriteKinBodyType(pbody,kmout->kmodel);

        // collision data
        {
            domExtraRef pextra = daeSafeCast<domExtra>(kmout->kmodel->add(COLLADA_ELEMENT_EXTRA));
            pextra->setType("collision");
            domTechniqueRef ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
            ptec->setProfile("OpenRAVE");
            FOREACHC(itadjacent,pbody->_vForcedAdjacentLinks) {
                KinBody::LinkPtr plink0 = pbody->GetLink(itadjacent->first);
                KinBody::LinkPtr plink1 = pbody->GetLink(itadjacent->second);
                if( !!plink0 && !!plink1 ) {
                    daeElementRef pignore = ptec->add("ignore_link_pair");
                    pignore->setAttribute("link0",(kmodelid + string("/") + kmout->vlinksids.at(plink0->GetIndex())).c_str());
                    pignore->setAttribute("link1",(kmodelid + string("/") + kmout->vlinksids.at(plink1->GetIndex())).c_str());
                }
            }
        }

        // create the formulas for all mimic joints
        std::map<std::string,std::string> mapjointnames;
        FOREACHC(itjoint,vjoints) {
            mapjointnames[str(boost::format("<csymbol>%s</csymbol>")%itjoint->second->GetName())] = str(boost::format("<csymbol encoding=\"COLLADA\">%s/joint%d</csymbol>")%kmodel->getID()%itjoint->first);
        }

        FOREACHC(itjoint, vjoints) {
            KinBody::JointConstPtr pjoint = itjoint->second;
            if( !pjoint->IsMimic() ) {
                continue;
            }
            if( pjoint->GetDOF() > 1 ) {
                RAVELOG_WARN("collada writer might not support multi-dof joint formulas...");
            }
            domFormulaRef pf = daeSafeCast<domFormula>(ktec->add(COLLADA_ELEMENT_FORMULA));
            string formulaid = str(boost::format("joint%d.formula")%itjoint->first);
            pf->setSid(formulaid.c_str());
            domCommon_float_or_paramRef ptarget = daeSafeCast<domCommon_float_or_param>(pf->add(COLLADA_ELEMENT_TARGET));
            string targetjointid = str(boost::format("%s/joint%d")%kmodel->getID()%itjoint->first);
            daeSafeCast<domCommon_param>(ptarget->add(COLLADA_TYPE_PARAM))->setValue(targetjointid.c_str());

            int iaxis = 0;
            boost::array<string,3> sequations;
            for(int itype = 0; itype < 3; ++itype) {
                sequations[itype] = pjoint->GetMimicEquation(iaxis,itype,"mathml");
                FOREACH(itmapping,mapjointnames) {
                    boost::algorithm::replace_all(sequations[itype],itmapping->first,itmapping->second);
                }
            }
            boost::array<const char*,3> sequationids = { { "position","first_partial","second_partial"}};

            domTechniqueRef pftec = daeSafeCast<domTechnique>(pf->add(COLLADA_ELEMENT_TECHNIQUE));
            pftec->setProfile("OpenRAVE");
            // save position equation
            daeElementRef poselt = pftec->add("equation");
            poselt->setAttribute("type",sequationids[0]);
            XMLtoDAE::Parse(poselt, sequations[0].c_str(), sequations[0].size());

            // save partial derivative equations
            for(int itype = 1; itype < 3; ++itype) {
                if( sequations[itype].size() == 0 ) {
                    continue;
                }
                size_t offset = 0;
                FOREACHC(itdofformat, pjoint->_vmimic[iaxis]->_vdofformat) {
                    if(offset<sequations[itype].size()) {
                        daeElementRef pelt = pftec->add("equation");
                        pelt->setAttribute("type",sequationids[itype]);
                        KinBody::JointPtr pmimic = itdofformat->jointindex < (int)pbody->GetJoints().size() ? pbody->GetJoints().at(itdofformat->jointindex) : pbody->GetPassiveJoints().at(itdofformat->jointindex-(int)pbody->GetJoints().size());
                        std::string smimicid = str(boost::format("%s/joint%d")%kmodel->getID()%pmimic->GetJointIndex());
                        pelt->setAttribute("target",smimicid.c_str());
                        offset += XMLtoDAE::Parse(pelt, sequations[itype].c_str()+offset, sequations[itype].size()-offset);
                        if( offset == 0 ) {
                            RAVELOG_WARN(str(boost::format("failed to parse joint %s first partial: %s\n")%pjoint->GetName()%sequations[itype]));
                            break;
                        }
                    }
                }
            }
            domFormula_techniqueRef pfdefaulttec = daeSafeCast<domFormula_technique>(pf->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
            XMLtoDAE::Parse(pfdefaulttec, sequations[0].c_str(), sequations[0].size());
        }
        _AddKinematics_model(pbody,kmout);
        return kmout;
    }

    virtual boost::shared_ptr<physics_model_output> WritePhysics_model(KinBodyPtr pbody)
    {
        boost::shared_ptr<physics_model_output> pmout = _GetPhysics_model(pbody);
        if( !!pmout ) {
            return pmout;
        }
        pmout.reset(new physics_model_output());
        pmout->pmodel = daeSafeCast<domPhysics_model>(_physicsModelsLib->add(COLLADA_ELEMENT_PHYSICS_MODEL));
        string pmodelid = str(boost::format("pmodel%d")%pbody->GetEnvironmentId());
        pmout->pmodel->setId(pmodelid.c_str());
        pmout->pmodel->setName(pbody->GetName().c_str());
        Transform tbaseinv = pbody->GetTransform().inverse();
        FOREACHC(itlink,pbody->GetLinks()) {
            domRigid_bodyRef rigid_body = daeSafeCast<domRigid_body>(pmout->pmodel->add(COLLADA_ELEMENT_RIGID_BODY));
            string rigidsid = str(boost::format("rigid%d")%(*itlink)->GetIndex());
            pmout->vrigidbodysids.push_back(rigidsid);
            rigid_body->setSid(rigidsid.c_str());
            rigid_body->setName((*itlink)->GetName().c_str());
            domRigid_body::domTechnique_commonRef ptec = daeSafeCast<domRigid_body::domTechnique_common>(rigid_body->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
            domTargetable_floatRef mass = daeSafeCast<domTargetable_float>(ptec->add(COLLADA_ELEMENT_MASS));
            mass->setValue((*itlink)->GetMass());
            Transform tlink0 = _GetLinkTransformZero(*itlink);
            _SetVector3(daeSafeCast<domTargetable_float3>(ptec->add(COLLADA_ELEMENT_INERTIA))->getValue(),(*itlink)->GetPrincipalMomentsOfInertia());
            _WriteTransformation(ptec->add(COLLADA_ELEMENT_MASS_FRAME), tbaseinv*tlink0*(*itlink)->GetLocalMassFrame());
            daeSafeCast<domRigid_body::domTechnique_common::domDynamic>(ptec->add(COLLADA_ELEMENT_DYNAMIC))->setValue(xsBoolean(!(*itlink)->IsStatic()));

            if( IsWrite("geometry") ) {
                // create a shape for every geometry
                int igeom = 0;
                FOREACHC(itgeom, (*itlink)->GetGeometries()) {
                    domRigid_body::domTechnique_common::domShapeRef pdomshape = daeSafeCast<domRigid_body::domTechnique_common::domShape>(ptec->add(COLLADA_ELEMENT_SHAPE));
                    // there is a weird bug here where _WriteTranformation will fail to create rotate/translate elements in instance_geometry is created first... (is this part of the spec?)
                    _WriteTransformation(pdomshape,tbaseinv*tlink0*(*itgeom)->GetTransform());
                    domInstance_geometryRef pinstgeom = daeSafeCast<domInstance_geometry>(pdomshape->add(COLLADA_ELEMENT_INSTANCE_GEOMETRY));
                    pinstgeom->setUrl(daeURI(*pinstgeom,string("#")+_GetGeometryId(*itlink,igeom)));
                    ++igeom;
                }
            }
        }
        return pmout;
    }

    /// \brief Write geometry properties
    /// \param geom Link geometry
    /// \param parentid Parent Identifier
    virtual domGeometryRef WriteGeometry(KinBody::Link::GeometryConstPtr geom, const string& parentid)
    {
        const KinBody::Link::TRIMESH& mesh = geom->GetCollisionMesh();
        Transform t = geom->GetTransform();

        string effid = parentid+string("_eff");
        string matid = parentid+string("_mat");

        domEffectRef pdomeff = WriteEffect(geom);
        pdomeff->setId(effid.c_str());

        domMaterialRef pdommat = daeSafeCast<domMaterial>(_materialsLib->add(COLLADA_ELEMENT_MATERIAL));
        pdommat->setId(matid.c_str());
        domInstance_effectRef pdominsteff = daeSafeCast<domInstance_effect>(pdommat->add(COLLADA_ELEMENT_INSTANCE_EFFECT));
        pdominsteff->setUrl((string("#")+effid).c_str());

        domGeometryRef pdomgeom = daeSafeCast<domGeometry>(_geometriesLib->add(COLLADA_ELEMENT_GEOMETRY));
        {
            pdomgeom->setId(parentid.c_str());
            domMeshRef pdommesh = daeSafeCast<domMesh>(pdomgeom->add(COLLADA_ELEMENT_MESH));
            {
                domSourceRef pvertsource = daeSafeCast<domSource>(pdommesh->add(COLLADA_ELEMENT_SOURCE));
                {
                    pvertsource->setId((parentid+string("_positions")).c_str());

                    domFloat_arrayRef parray = daeSafeCast<domFloat_array>(pvertsource->add(COLLADA_ELEMENT_FLOAT_ARRAY));
                    parray->setId((parentid+string("_positions-array")).c_str());
                    parray->setCount(3*mesh.vertices.size());
                    parray->setDigits(std::numeric_limits<OpenRAVE::dReal>::digits10+1);
                    parray->getValue().setCount(3*mesh.vertices.size());

                    for(size_t ind = 0; ind < mesh.vertices.size(); ++ind) {
                        Vector v = t*mesh.vertices[ind];
                        parray->getValue()[3*ind+0] = v.x;
                        parray->getValue()[3*ind+1] = v.y;
                        parray->getValue()[3*ind+2] = v.z;
                    }

                    domSource::domTechnique_commonRef psourcetec = daeSafeCast<domSource::domTechnique_common>(pvertsource->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
                    domAccessorRef pacc = daeSafeCast<domAccessor>(psourcetec->add(COLLADA_ELEMENT_ACCESSOR));
                    pacc->setCount(mesh.vertices.size());
                    pacc->setSource(daeURI(*pacc, string("#")+parentid+string("_positions-array")));
                    pacc->setStride(3);

                    domParamRef px = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                    px->setName("X"); px->setType("float");
                    domParamRef py = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                    py->setName("Y"); py->setType("float");
                    domParamRef pz = daeSafeCast<domParam>(pacc->add(COLLADA_ELEMENT_PARAM));
                    pz->setName("Z"); pz->setType("float");
                }

                domVerticesRef pverts = daeSafeCast<domVertices>(pdommesh->add(COLLADA_ELEMENT_VERTICES));
                {
                    pverts->setId((parentid+string("_vertices")).c_str());
                    domInput_localRef pvertinput = daeSafeCast<domInput_local>(pverts->add(COLLADA_ELEMENT_INPUT));
                    pvertinput->setSemantic("POSITION");
                    pvertinput->setSource(domUrifragment(*pvertsource, string("#")+parentid+string("_positions")));
                }

                domTrianglesRef ptris = daeSafeCast<domTriangles>(pdommesh->add(COLLADA_ELEMENT_TRIANGLES));
                {
                    ptris->setCount(mesh.indices.size()/3);
                    ptris->setMaterial("mat0");

                    domInput_local_offsetRef pvertoffset = daeSafeCast<domInput_local_offset>(ptris->add(COLLADA_ELEMENT_INPUT));
                    pvertoffset->setSemantic("VERTEX");
                    pvertoffset->setOffset(0);
                    pvertoffset->setSource(domUrifragment(*pverts, string("#")+parentid+string("_vertices")));
                    domPRef pindices = daeSafeCast<domP>(ptris->add(COLLADA_ELEMENT_P));
                    pindices->getValue().setCount(mesh.indices.size());
                    for(size_t ind = 0; ind < mesh.indices.size(); ++ind) {
                        pindices->getValue()[ind] = mesh.indices[ind];
                    }
                }
            }
        }

        {
            stringstream ss; ss << std::setprecision(std::numeric_limits<OpenRAVE::dReal>::digits10+1);
            // write the geometry_info tag
            domExtraRef pextra = daeSafeCast<domExtra>(pdomgeom->add(COLLADA_ELEMENT_EXTRA));
            pextra->setType("geometry_info");
            domTechniqueRef ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
            ptec->setProfile("OpenRAVE");
            Transform tlocalgeom = geom->GetTransform();
            switch(geom->GetType()) {
            case KinBody::Link::GeomBox:
                ss << geom->GetBoxExtents().x << " " << geom->GetBoxExtents().y << " " << geom->GetBoxExtents().z;
                ptec->add("box")->add("half_extents")->setCharData(ss.str());
                break;
            case KinBody::Link::GeomSphere:
                ptec->add("sphere")->add("radius")->setCharData(ss.str());
                break;
            case KinBody::Link::GeomCylinder: {
                daeElementRef pcylinder = ptec->add("cylinder");
                ss << geom->GetCylinderRadius() << " " << geom->GetCylinderRadius();
                pcylinder->add("radius")->setCharData(ss.str());
                pcylinder->add("height")->setCharData(boost::lexical_cast<std::string>(geom->GetCylinderHeight()));
                // collada cylinder is oriented toward y-axis while openrave is toward z-axis
                Transform trot(quatRotateDirection(Vector(0,1,0),Vector(0,0,1)),Vector());
                tlocalgeom = tlocalgeom * trot;
                break;
            }
            case KinBody::Link::GeomNone:
            case KinBody::Link::GeomTrimesh:
                // don't add anything
                break;
            }
            // add the coordinate system
            _WriteTransformation(ptec, tlocalgeom);
            ptec->add("visible")->add("bool")->setCharData(geom->IsVisible() ? "true" : "false");
        }
        return pdomgeom;
    }

    /// Write light effect
    /// vambient    Ambient light color
    /// vdiffuse    Diffuse light color
    virtual domEffectRef WriteEffect(const KinBody::Link::GeometryConstPtr geom)
    {
        domEffectRef pdomeff = daeSafeCast<domEffect>(_effectsLib->add(COLLADA_ELEMENT_EFFECT));

        domProfile_commonRef pprofile = daeSafeCast<domProfile_common>(pdomeff->add(COLLADA_ELEMENT_PROFILE_COMMON));
        domProfile_common::domTechniqueRef ptec = daeSafeCast<domProfile_common::domTechnique>(pprofile->add(COLLADA_ELEMENT_TECHNIQUE));

        domProfile_common::domTechnique::domPhongRef pphong = daeSafeCast<domProfile_common::domTechnique::domPhong>(ptec->add(COLLADA_ELEMENT_PHONG));

        domFx_common_color_or_textureRef pambient = daeSafeCast<domFx_common_color_or_texture>(pphong->add(COLLADA_ELEMENT_AMBIENT));
        domFx_common_color_or_texture::domColorRef pambientcolor = daeSafeCast<domFx_common_color_or_texture::domColor>(pambient->add(COLLADA_ELEMENT_COLOR));
        _SetVector4(pambientcolor->getValue(), geom->GetAmbientColor());

        domFx_common_color_or_textureRef pdiffuse = daeSafeCast<domFx_common_color_or_texture>(pphong->add(COLLADA_ELEMENT_DIFFUSE));
        domFx_common_color_or_texture::domColorRef pdiffusecolor = daeSafeCast<domFx_common_color_or_texture::domColor>(pdiffuse->add(COLLADA_ELEMENT_COLOR));
        _SetVector4(pdiffusecolor->getValue(), geom->GetDiffuseColor());

        domFx_common_float_or_paramRef ptransparency = daeSafeCast<domFx_common_float_or_param>(pphong->add(COLLADA_ELEMENT_TRANSPARENCY));
        daeSafeCast<domFx_common_float_or_param::domFloat>(ptransparency->add(COLLADA_ELEMENT_FLOAT))->setValue(geom->GetTransparency());
        return pdomeff;
    }

private:

    /// \brief save all the loaded scene models and their current state.
    virtual void _CreateScene()
    {
        // Create kinematics scene
        _scene.kscene = daeSafeCast<domKinematics_scene>(_kinematicsScenesLib->add(COLLADA_ELEMENT_KINEMATICS_SCENE));
        _scene.kscene->setId("kscene");
        _scene.kscene->setName("OpenRAVE Kinematics Scene");
        // Create instance kinematics scene
        _scene.kiscene = daeSafeCast<domInstance_kinematics_scene>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_KINEMATICS_SCENE ));
        _scene.kiscene->setUrl(str(boost::format("#%s")%_scene.kscene->getId()).c_str());
        _scene.kiscene->setSid(str(boost::format("%s_inst")%_scene.kscene->getId()).c_str());

        if( IsWrite("visual") ) {
            // Create visual scene
            _scene.vscene = daeSafeCast<domVisual_scene>(_visualScenesLib->add(COLLADA_ELEMENT_VISUAL_SCENE));
            _scene.vscene->setId("vscene");
            _scene.vscene->setName("OpenRAVE Visual Scene");

            // Create instance visual scene
            _scene.viscene = daeSafeCast<domInstance_with_extra>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_VISUAL_SCENE ));
            _scene.viscene->setUrl(str(boost::format("#%s")%_scene.vscene->getId()).c_str());
            _scene.viscene->setSid(str(boost::format("%s_inst")%_scene.vscene->getId()).c_str());
        }
        if( IsWrite("physics") ) {
            // Create physic scene
            _scene.pscene = daeSafeCast<domPhysics_scene>(_physicsScenesLib->add(COLLADA_ELEMENT_PHYSICS_SCENE));
            _scene.pscene->setId("pscene");
            _scene.pscene->setName("OpenRAVE Physics Scene");

            // Create instance physics scene
            _scene.piscene = daeSafeCast<domInstance_with_extra>(_globalscene->add( COLLADA_ELEMENT_INSTANCE_PHYSICS_SCENE ));
            _scene.piscene->setUrl(str(boost::format("#%s")%_scene.pscene->getId()).c_str());
            _scene.piscene->setSid(str(boost::format("%s_inst")%_scene.pscene->getId()).c_str());
        }

    }

    /** \brief Write link of a kinematic body

        \param link Link to write
        \param pkinparent Kinbody parent
        \param pnodeparent Node parent
        \param strModelUri
        \param vjoints Vector of joints
     */
    virtual LINKOUTPUT _WriteLink(KinBody::LinkConstPtr plink, daeElementRef pkinparent, domNodeRef pnodeparent, const string& strModelUri, const vector<pair<int, KinBody::JointConstPtr> >& vjoints)
    {
        RAVELOG_VERBOSE(str(boost::format("writing link %s, node parent id=%s")%plink->GetName()%pnodeparent->getId()));
        LINKOUTPUT out;
        string linksid = _GetLinkSid(plink);
        domLinkRef pdomlink = daeSafeCast<domLink>(pkinparent->add(COLLADA_ELEMENT_LINK));
        pdomlink->setName(plink->GetName().c_str());
        pdomlink->setSid(linksid.c_str());

        domNodeRef pnode;
        if( IsWrite("visual") ) {
            pnode = daeSafeCast<domNode>(pnodeparent->add(COLLADA_ELEMENT_NODE));
            std::string nodeid = _GetNodeId(plink);
            pnode->setId( nodeid.c_str() );
            string nodesid = _GetNodeSid(plink);
            pnode->setSid(nodesid.c_str());
            pnode->setName(plink->GetName().c_str());

            if( IsWrite("geometry") ) {
                int igeom = 0;
                FOREACHC(itgeom, plink->GetGeometries()) {
                    string geomid = _GetGeometryId(plink,igeom);
                    igeom++;
                    domGeometryRef pdomgeom = WriteGeometry(*itgeom, geomid);
                    domInstance_geometryRef pinstgeom = daeSafeCast<domInstance_geometry>(pnode->add(COLLADA_ELEMENT_INSTANCE_GEOMETRY));
                    pinstgeom->setUrl((string("#")+geomid).c_str());

                    domBind_materialRef pmat = daeSafeCast<domBind_material>(pinstgeom->add(COLLADA_ELEMENT_BIND_MATERIAL));
                    domBind_material::domTechnique_commonRef pmattec = daeSafeCast<domBind_material::domTechnique_common>(pmat->add(COLLADA_ELEMENT_TECHNIQUE_COMMON));
                    domInstance_materialRef pinstmat = daeSafeCast<domInstance_material>(pmattec->add(COLLADA_ELEMENT_INSTANCE_MATERIAL));
                    pinstmat->setTarget(daeURI(*pinstmat, string("#")+geomid+string("_mat")));
                    pinstmat->setSymbol("mat0");
                }
            }
        }

        // look for all the child links
        FOREACHC(itjoint, vjoints) {
            KinBody::JointConstPtr pjoint = itjoint->second;
            if( pjoint->GetHierarchyParentLink() != plink ) {
                continue;
            }
            KinBody::LinkPtr pchild = pjoint->GetHierarchyChildLink();
            if( !pchild ) {
                continue;
            }

            domLink::domAttachment_fullRef pattfull = daeSafeCast<domLink::domAttachment_full>(pdomlink->add(COLLADA_TYPE_ATTACHMENT_FULL));
            string jointid = str(boost::format("%s/joint%d")%strModelUri%itjoint->first);
            pattfull->setJoint(jointid.c_str());

            LINKOUTPUT childinfo = _WriteLink(pchild, pattfull, pnode, strModelUri, vjoints);
            out.listusedlinks.insert(out.listusedlinks.end(),childinfo.listusedlinks.begin(),childinfo.listusedlinks.end());

            _WriteTransformation(pattfull, pjoint->GetInternalHierarchyLeftTransform());
            _WriteTransformation(childinfo.plink, pjoint->GetInternalHierarchyRightTransform());

            if( IsWrite("visual") ) {
                _WriteTransformation(childinfo.pnode, pjoint->GetInternalHierarchyRightTransform());

                for(int iaxis = 0; iaxis < pjoint->GetDOF(); ++iaxis) {
                    string jointnodesid = _GetJointNodeSid(pjoint,iaxis);
                    if( pjoint->IsRevolute(iaxis) ) {
                        domRotateRef protate = daeSafeCast<domRotate>(childinfo.pnode->add(COLLADA_ELEMENT_ROTATE,0));
                        protate->setSid(jointnodesid.c_str());
                        protate->getValue().setCount(4);
                        protate->getValue()[0] = pjoint->GetInternalHierarchyAxis(0).x;
                        protate->getValue()[1] = pjoint->GetInternalHierarchyAxis(0).y;
                        protate->getValue()[2] = pjoint->GetInternalHierarchyAxis(0).z;
                        protate->getValue()[3] = 0;
                    }
                    else if( pjoint->IsPrismatic(iaxis) ) {
                        domTranslateRef ptrans = daeSafeCast<domTranslate>(childinfo.pnode->add(COLLADA_ELEMENT_TRANSLATE,0));
                        ptrans->setSid(jointnodesid.c_str());
                        ptrans->getValue().setCount(3);
                        ptrans->getValue()[0] = 0;
                        ptrans->getValue()[1] = 0;
                        ptrans->getValue()[2] = 0;
                    }
                    else {
                        RAVELOG_WARN(str(boost::format("unsupported joint type specified 0x%x")%pjoint->GetType()));
                        continue;
                    }
                }
                _WriteTransformation(childinfo.pnode, pjoint->GetInternalHierarchyLeftTransform());
            }
        }

        out.listusedlinks.push_back(make_pair(plink->GetIndex(),linksid));
        out.plink = pdomlink;
        out.pnode = pnode;
        return out;
    }

    void _SetRotate(domTargetable_float4Ref prot, const Vector& rot)
    {
        prot->getValue().setCount(4);
        Vector vaxisangle = axisAngleFromQuat(rot);
        dReal fnorm = RaveSqrt(vaxisangle.lengthsqr3());
        if( fnorm > 0 ) {
            prot->getValue()[0] = vaxisangle.x/fnorm;
            prot->getValue()[1] = vaxisangle.y/fnorm;
            prot->getValue()[2] = vaxisangle.z/fnorm;
            prot->getValue()[3] = fnorm*(180.0/PI);
        }
        else {
            prot->getValue()[0] = 1;
            prot->getValue()[1] = 0;
            prot->getValue()[2] = 0;
            prot->getValue()[3] = 0;
        }
    }

    /// \brief Write transformation
    /// \param pelt Element to transform
    /// \param t Transform to write
    /// \param bwritesids if true, will write 'translate' and 'rotate' sids
    void _WriteTransformation(daeElementRef pelt, const Transform& t, bool bwritesids=false)
    {
        domRotateRef protate = daeSafeCast<domRotate>(pelt->add(COLLADA_ELEMENT_ROTATE,0));
        _SetRotate(protate,t.rot);
        domTranslateRef ptranslate = daeSafeCast<domTranslate>(pelt->add(COLLADA_ELEMENT_TRANSLATE,0));
        _SetVector3(ptranslate->getValue(),t.trans);
        if( bwritesids ) {
            protate->setSid("rotate");
            ptranslate->setSid("translate");
        }
    }

    /// \brief binding in instance_kinematics_scene
    void _WriteBindingsInstance_kinematics_scene(domInstance_kinematics_sceneRef ikscene, KinBodyConstPtr pbody, const std::vector<axis_sids>& vaxissids, const std::vector<std::pair<std::string,std::string> >& vkinematicsbindings)
    {
        FOREACHC(it, vkinematicsbindings) {
            domBind_kinematics_modelRef pmodelbind = daeSafeCast<domBind_kinematics_model>(ikscene->add(COLLADA_ELEMENT_BIND_KINEMATICS_MODEL));
            pmodelbind->setNode(it->second.c_str());
            daeSafeCast<domCommon_param>(pmodelbind->add(COLLADA_ELEMENT_PARAM))->setValue(it->first.c_str());
        }
        FOREACHC(it, vaxissids) {
            domBind_joint_axisRef pjointbind = daeSafeCast<domBind_joint_axis>(ikscene->add(COLLADA_ELEMENT_BIND_JOINT_AXIS));
            pjointbind->setTarget(it->jointnodesid.c_str());
            daeSafeCast<domCommon_param>(pjointbind->add(COLLADA_ELEMENT_AXIS)->add(COLLADA_TYPE_PARAM))->setValue(it->axissid.c_str());
            daeElementRef pvalueelt = pjointbind->add(COLLADA_ELEMENT_VALUE);
            if( it->valuesid.size() > 0 ) {
                daeSafeCast<domCommon_param>(pvalueelt->add(COLLADA_TYPE_PARAM))->setValue(it->valuesid.c_str());
            }
            else {
                pvalueelt->add(COLLADA_TYPE_FLOAT)->setCharData(boost::lexical_cast<std::string>(it->value));
            }
        }
    }

    /// \brief writes the dynamic rigid constr
    void _WriteDynamicRigidConstraints(domInstance_with_extraRef piscene, const std::list<boost::shared_ptr<instance_articulated_system_output> >& listModelDatabase)
    {
        domTechniqueRef ptec;
        // go through every robot and check if it has grabbed bodies
        std::vector<KinBodyPtr> vGrabbedBodies;
        size_t idynamicconstraint = 0;
        FOREACHC(itias, listModelDatabase) {
            if( (*itias)->pbody->IsRobot() ) {
                RobotBasePtr probot = RaveInterfaceCast<RobotBase>((*itias)->pbody);
                probot->GetGrabbed(vGrabbedBodies);
                FOREACHC(itgrabbed,vGrabbedBodies) {
                    boost::shared_ptr<instance_articulated_system_output> grabbedias;
                    FOREACHC(itias2,listModelDatabase) {
                        if( (*itias2)->pbody == *itgrabbed ) {
                            grabbedias = *itias2;
                            break;
                        }
                    }
                    if( !grabbedias ) {
                        RAVELOG_WARN(str(boost::format("grabbed body %s not saved in COLLADA so cannot reference")%(*itgrabbed)->GetName()));
                        continue;
                    }

                    KinBody::LinkPtr pgrabbinglink = probot->IsGrabbing(*itgrabbed);
                    if( !ptec ) {
                        domExtraRef pextra = daeSafeCast<domExtra>(piscene->add(COLLADA_ELEMENT_EXTRA));
                        pextra->setType("dynamic_rigid_constraints");
                        ptec = daeSafeCast<domTechnique>(pextra->add(COLLADA_ELEMENT_TECHNIQUE));
                        ptec->setProfile("OpenRAVE");
                    }

                    daeElementRef pconstraint = ptec->add("rigid_constraint");
                    pconstraint->setAttribute("sid",str(boost::format("grab%d")%idynamicconstraint).c_str());
                    idynamicconstraint++;
                    string rigid_body = str(boost::format("%s/%s")%(*itias)->ipmout->ipm->getSid()%(*itias)->ipmout->pmout->vrigidbodysids.at(pgrabbinglink->GetIndex()));
                    pconstraint->add("ref_attachment")->setAttribute("rigid_body",rigid_body.c_str());
                    rigid_body = str(boost::format("%s/%s")%grabbedias->ipmout->ipm->getSid()%grabbedias->ipmout->pmout->vrigidbodysids.at(0));
                    pconstraint->add("attachment")->setAttribute("rigid_body",rigid_body.c_str());
                }
            }
        }
    }

    /// Set vector of four elements
    template <typename T> static void _SetVector4(T& t, const Vector& v) {
        t.setCount(4);
        t[0] = v.x;
        t[1] = v.y;
        t[2] = v.z;
        t[3] = v.w;
    }

    /// Set vector of three elements
    template <typename T> static void _SetVector3(T& t, const Vector& v) {
        t.setCount(3);
        t[0] = v.x;
        t[1] = v.y;
        t[2] = v.z;
    }

    virtual void _AddKinematics_model(KinBodyPtr pbody, boost::shared_ptr<kinematics_model_output> kmout) {
        FOREACH(it, _listkinbodies) {
            if(( it->uri == pbody->GetURI()) &&( it->kinematicsgeometryhash == pbody->GetKinematicsGeometryHash()) ) {
                BOOST_ASSERT(!it->kmout);
                it->kmout = kmout;
                return;
            }
        }
        kinbody_models cache;
        cache.uri = pbody->GetURI();
        cache.kinematicsgeometryhash = pbody->GetKinematicsGeometryHash();
        cache.kmout = kmout;
        _listkinbodies.push_back(cache);
    }

    virtual boost::shared_ptr<kinematics_model_output> _GetKinematics_model(KinBodyPtr pbody) {
        FOREACH(it, _listkinbodies) {
            if(( it->uri == pbody->GetURI()) &&( it->kinematicsgeometryhash == pbody->GetKinematicsGeometryHash()) ) {
                return it->kmout;
            }
        }
        return boost::shared_ptr<kinematics_model_output>();
    }

    virtual void _AddPhysics_model(KinBodyPtr pbody, boost::shared_ptr<physics_model_output> pmout) {
        FOREACH(it, _listkinbodies) {
            if(( it->uri == pbody->GetURI()) &&( it->kinematicsgeometryhash == pbody->GetKinematicsGeometryHash()) ) {
                BOOST_ASSERT(!it->pmout);
                it->pmout = pmout;
                return;
            }
        }
        kinbody_models cache;
        cache.uri = pbody->GetURI();
        cache.kinematicsgeometryhash = pbody->GetKinematicsGeometryHash();
        cache.pmout = pmout;
        _listkinbodies.push_back(cache);
    }

    virtual boost::shared_ptr<physics_model_output> _GetPhysics_model(KinBodyPtr pbody) {
        FOREACH(it, _listkinbodies) {
            if(( it->uri == pbody->GetURI()) &&( it->kinematicsgeometryhash == pbody->GetKinematicsGeometryHash()) ) {
                return it->pmout;
            }
        }
        return boost::shared_ptr<physics_model_output>();
    }

    virtual std::string _GetNodeId(KinBodyConstPtr pbody) {
        return str(boost::format("visual%d")%pbody->GetEnvironmentId());
    }
    virtual std::string _GetNodeId(KinBody::LinkConstPtr plink) {
        return str(boost::format("v%d.node%d")%plink->GetParent()->GetEnvironmentId()%plink->GetIndex());
    }
    virtual std::string _GetNodeSid(KinBody::LinkConstPtr plink) {
        return str(boost::format("node%d")%plink->GetIndex());
    }

    virtual std::string _GetLinkSid(KinBody::LinkConstPtr plink) {
        return str(boost::format("link%d")%plink->GetIndex());
    }

    virtual std::string _GetGeometryId(KinBody::LinkConstPtr plink, int igeom) {
        return str(boost::format("g%d_%s_geom%d")%plink->GetParent()->GetEnvironmentId()%_GetLinkSid(plink)%igeom);
    }
    virtual std::string _GetJointNodeSid(KinBody::JointConstPtr pjoint, int iaxis) {
        int index = pjoint->GetJointIndex();
        if( index < 0 ) {     // must be passive
            index = (int)pjoint->GetParent()->GetJoints().size();
            FOREACHC(itpjoint,pjoint->GetParent()->GetPassiveJoints()) {
                if( pjoint == *itpjoint ) {
                    break;
                }
                ++index;
            }
        }
        return str(boost::format("node_joint%d_axis%d")%index%iaxis);
    }

    virtual std::string _GetKinematicsModelId(KinBodyConstPtr pbody) {
        return str(boost::format("kmodel%d")%pbody->GetEnvironmentId());
    }

    /// \brief compute the link transform when all joints are zero (regardless of mimic joints). This is the state
    /// that the entire robot is stored in
    virtual Transform _GetLinkTransformZero(KinBody::LinkConstPtr plink)
    {
        KinBodyConstPtr pbody = plink->GetParent();
        std::vector<KinBody::JointPtr> vjoints;
        pbody->GetChain(0,plink->GetIndex(),vjoints);
        Transform t = pbody->GetTransform();
        FOREACHC(itjoint,vjoints) {
            t *= (*itjoint)->GetInternalHierarchyLeftTransform() * (*itjoint)->GetInternalHierarchyRightTransform();
        }
        return t;
    }

    virtual void handleError( daeString msg )
    {
        RAVELOG_ERROR("COLLADA error: %s\n", msg);
    }

    virtual void handleWarning( daeString msg )
    {
        RAVELOG_WARN("COLLADA warning: %s\n", msg);
    }

    virtual bool IsWrite(const std::string& type)
    {
        return _setSkipWriteOptions.find(type) == _setSkipWriteOptions.end();
    }

    boost::shared_ptr<DAE> _dae;
    domCOLLADA* _dom;
    daeDocument* _doc;
    domCOLLADA::domSceneRef _globalscene;
    domLibrary_visual_scenesRef _visualScenesLib;
    domLibrary_kinematics_scenesRef _kinematicsScenesLib;
    domLibrary_kinematics_modelsRef _kinematicsModelsLib;
    domLibrary_articulated_systemsRef _articulatedSystemsLib;
    domLibrary_physics_scenesRef _physicsScenesLib;
    domLibrary_physics_modelsRef _physicsModelsLib;
    domLibrary_materialsRef _materialsLib;
    domLibrary_effectsRef _effectsLib;
    domLibrary_geometriesRef _geometriesLib;
    domTechniqueRef _sensorsLib, _actuatorsLib;     ///< custom libraries
    SCENE _scene;
    dReal _globalunit; ///< how many real-world meters in one distance unit
    EnvironmentBaseConstPtr _penv;
    std::list<kinbody_models> _listkinbodies;
    std::string _vForceResolveOpenRAVEScheme; ///< if specified, writer will attempt to convert a local system URI (**file:/**) to a a relative path with respect to $OPENRAVE_DATA paths and use **customscheme** as the scheme
    std::list<std::string> _listExternalRefExports; ///< body names to try to export externally
    std::list<std::string> _listIgnoreExternalURIs; ///< don't use these URIs for external indexing
    std::list<std::string> _listForceWriteOptions;
    std::set<std::string> _setSkipWriteOptions;

    bool _bExternalRefAllBodies; ///< if true, attempts to externally write all bodies
    bool _bForceWriteAll; ///< if true, attemps to write all modifiable data to externally saved bodies
};

// register for typeof (MSVC only)
#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(ColladaWriter::LINKOUTPUT)
BOOST_TYPEOF_REGISTER_TYPE(ColladaWriter::kinematics_model_output)
BOOST_TYPEOF_REGISTER_TYPE(ColladaWriter::instance_kinematics_model_output)
BOOST_TYPEOF_REGISTER_TYPE(ColladaWriter::articulated_system_output)
#endif

void RaveWriteColladaFile(EnvironmentBasePtr penv, const string& filename, const AttributesList& atts)
{
    ColladaWriter writer(penv, atts);
    writer.Init("openrave_snapshot");
    if( !writer.Write() ) {
        throw openrave_exception("ColladaWriter::Write(EnvironmentBasePtr) failed");
    }
    writer.Save(filename);
}

void RaveWriteColladaFile(KinBodyPtr pbody, const string& filename, const AttributesList& atts)
{
    ColladaWriter writer(pbody->GetEnv(),atts);
    writer.Init("openrave_snapshot");
    if( !writer.Write(pbody) ) {
        throw openrave_exception("ColladaWriter::Write(KinBodyPtr) failed");
    }
    writer.Save(filename);
}

void RaveWriteColladaFile(const std::list<KinBodyPtr>& listbodies, const std::string& filename,const AttributesList& atts)
{
    if( listbodies.size() > 0 ) {
        ColladaWriter writer(listbodies.front()->GetEnv(),atts);
        writer.Init("openrave_snapshot");
        if( !writer.Write(listbodies) ) {
            throw openrave_exception("ColladaWriter::Write(list<KinBodyPtr>) failed");
        }
        writer.Save(filename);
    }
}