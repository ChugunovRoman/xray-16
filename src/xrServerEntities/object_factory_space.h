////////////////////////////////////////////////////////////////////////////
//	Module 		: object_factory_space.h
//	Created 	: 30.06.2004
//  Modified 	: 30.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Object factory space
////////////////////////////////////////////////////////////////////////////

#pragma once

class IFactoryObject;
class CSE_Abstract;

namespace ObjectFactory
{
using ClientObjectBaseClass = IFactoryObject;
using ServerObjectBaseClass = CSE_Abstract;
// 32-frame-spikes.md (the no-op skip): told about every client object the factory creates, with its exact type,
// before _construct(); defined in object_factory_register.cpp, the only place that instantiates the object items
template <typename _client_type>
void client_object_created(_client_type* object);
// 37-destroy-relcase-smooth.md (the no-op skip of the script factory objects): every client class the C++ factory
// registers is noted by its exact type (CObjectFactory::add); an object the script factory creates (CObjectItemScript)
// is then looked up by typeid before _construct(). Both defined in object_factory_register.cpp.
template <typename _client_type>
void client_type_registered();
void script_client_object_created(ClientObjectBaseClass* object);
};
