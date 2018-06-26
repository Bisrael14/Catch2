/*
 *  Created by Martin on 19/07/2017
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include "catch_test_case_tracker.h"

#include "catch_enforce.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <memory>
#include <sstream>

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif

namespace Catch {
namespace TestCaseTracking {

    NameAndLocation::NameAndLocation( std::string const& _name, SourceLineInfo const& _location )
    :   name( _name ),
        location( _location )
    {}


    ITracker::~ITracker() = default;


    TrackerContext& TrackerContext::instance() {
        static TrackerContext s_instance;
        return s_instance;
    }

    ITracker& TrackerContext::startRun() {
        m_rootTracker = std::make_shared<SectionTracker>( NameAndLocation( "{root}", CATCH_INTERNAL_LINEINFO ), *this, nullptr );
        m_currentTracker = nullptr;
        m_runState = Executing;
        return *m_rootTracker;
    }

    void TrackerContext::endRun() {
        m_rootTracker.reset();
        m_currentTracker = nullptr;
        m_runState = NotStarted;
    }

    void TrackerContext::startCycle() {
        m_currentTracker = m_rootTracker.get();
        m_runState = Executing;
    }
    void TrackerContext::completeCycle() {
        m_runState = CompletedCycle;
    }

    bool TrackerContext::completedCycle() const {
        return m_runState == CompletedCycle;
    }
    ITracker& TrackerContext::currentTracker() {
        return *m_currentTracker;
    }
    void TrackerContext::setCurrentTracker( ITracker* tracker ) {
        m_currentTracker = tracker;
    }
    

    TrackerBase::TrackerBase( NameAndLocation const& nameAndLocation, TrackerContext& ctx, ITracker* parent )
    :   m_nameAndLocation( nameAndLocation ),
        m_ctx( ctx ),
        m_parent( parent )
    {}

    NameAndLocation const& TrackerBase::nameAndLocation() const {
        return m_nameAndLocation;
    }
    bool TrackerBase::isComplete() const {
        return m_runState == CompletedSuccessfully || m_runState == Failed;
    }
    bool TrackerBase::isSuccessfullyCompleted() const {
        return m_runState == CompletedSuccessfully;
    }
    bool TrackerBase::isOpen() const {
        return m_runState != NotStarted && !isComplete();
    }
    bool TrackerBase::hasChildren() const {
        return !m_children.empty();
    }


    void TrackerBase::addChild( ITrackerPtr const& child ) {
        m_children.push_back( child );
    }

    ITrackerPtr TrackerBase::findChild( NameAndLocation const& nameAndLocation ) {
        auto it = std::find_if( m_children.begin(), m_children.end(),
            [&nameAndLocation]( ITrackerPtr const& tracker ){
                return
                    tracker->nameAndLocation().location == nameAndLocation.location &&
                    tracker->nameAndLocation().name == nameAndLocation.name;
            } );
        return( it != m_children.end() )
            ? *it
            : nullptr;
    }
    ITracker& TrackerBase::parent() {
        assert( m_parent ); // Should always be non-null except for root
        return *m_parent;
    }

    void TrackerBase::openChild() {
        if( m_runState != ExecutingChildren ) {
            m_runState = ExecutingChildren;
            if( m_parent )
                m_parent->openChild();
        }
    }

    bool TrackerBase::isSectionTracker() const { return false; }
    bool TrackerBase::isIndexTracker() const { return false; }

    void TrackerBase::open() {
        m_runState = Executing;
        moveToThis();
        if( m_parent )
            m_parent->openChild();
    }

    void TrackerBase::close() {

        // Close any still open children (e.g. generators)
        while( &m_ctx.currentTracker() != this )
            m_ctx.currentTracker().close();

        switch( m_runState ) {
            case NeedsAnotherRun:
                break;

            case Executing:
                m_runState = CompletedSuccessfully;
                break;
            case ExecutingChildren:
                if( m_children.empty() || m_children.back()->isComplete() )
                    m_runState = CompletedSuccessfully;
                break;

            case NotStarted:
            case CompletedSuccessfully:
            case Failed:
                CATCH_INTERNAL_ERROR( "Illogical state: " << m_runState );

            default:
                CATCH_INTERNAL_ERROR( "Unknown state: " << m_runState );
        }
        moveToParent();
        m_ctx.completeCycle();
    }
    void TrackerBase::fail() {
        m_runState = Failed;
        if( m_parent )
            m_parent->markAsNeedingAnotherRun();
        moveToParent();
        m_ctx.completeCycle();
    }
    void TrackerBase::markAsNeedingAnotherRun() {
        m_runState = NeedsAnotherRun;
    }

    void TrackerBase::moveToParent() {
        assert( m_parent );
        m_ctx.setCurrentTracker( m_parent );
    }
    void TrackerBase::moveToThis() {
        m_ctx.setCurrentTracker( this );
    }

    SectionTracker::SectionTracker( NameAndLocation const& nameAndLocation, TrackerContext& ctx, ITracker* parent )
    :   TrackerBase( nameAndLocation, ctx, parent )
    {
        if( parent ) {
            while( !parent->isSectionTracker() )
                parent = &parent->parent();

            SectionTracker& parentSection = static_cast<SectionTracker&>( *parent );
            addNextFilters( parentSection.m_filters );
        }
    }

    bool SectionTracker::isSectionTracker() const { return true; }

    SectionTracker& SectionTracker::acquire( TrackerContext& ctx, NameAndLocation const& nameAndLocation ) {
        std::shared_ptr<SectionTracker> section;

        ITracker& currentTracker = ctx.currentTracker();
        if( ITrackerPtr childTracker = currentTracker.findChild( nameAndLocation ) ) {
            assert( childTracker );
            assert( childTracker->isSectionTracker() );
            section = std::static_pointer_cast<SectionTracker>( childTracker );
        }
        else {
            section = std::make_shared<SectionTracker>( nameAndLocation, ctx, &currentTracker );
            currentTracker.addChild( section );
        }
        if( !ctx.completedCycle() )
            section->tryOpen();
        return *section;
    }

    void SectionTracker::tryOpen() {
        if( !isComplete() && (m_filters.empty() || m_filters[0].empty() ||  m_filters[0] == m_nameAndLocation.name ) )
            open();
    }

    void SectionTracker::addInitialFilters( std::vector<std::string> const& filters ) {
        if( !filters.empty() ) {
            m_filters.push_back(""); // Root - should never be consulted
            m_filters.push_back(""); // Test Case - not a section filter
            m_filters.insert( m_filters.end(), filters.begin(), filters.end() );
        }
    }
    void SectionTracker::addNextFilters( std::vector<std::string> const& filters ) {
        if( filters.size() > 1 )
            m_filters.insert( m_filters.end(), ++filters.begin(), filters.end() );
    }

    IndexTracker::IndexTracker( NameAndLocation const& nameAndLocation, TrackerContext& ctx, ITracker* parent, int size )
    :   TrackerBase( nameAndLocation, ctx, parent ),
        m_size( size )
    {}

    bool IndexTracker::isIndexTracker() const { return true; }

    IndexTracker& IndexTracker::acquire( TrackerContext& ctx, NameAndLocation const& nameAndLocation, int size ) {
        std::shared_ptr<IndexTracker> tracker;

        ITracker& currentTracker = ctx.currentTracker();
        if( ITrackerPtr childTracker = currentTracker.findChild( nameAndLocation ) ) {
            assert( childTracker );
            assert( childTracker->isIndexTracker() );
            tracker = std::static_pointer_cast<IndexTracker>( childTracker );
        }
        else {
            tracker = std::make_shared<IndexTracker>( nameAndLocation, ctx, &currentTracker, size );
            currentTracker.addChild( tracker );
        }

        if( !ctx.completedCycle() && !tracker->isComplete() ) {
            if( tracker->m_runState != ExecutingChildren && tracker->m_runState != NeedsAnotherRun )
                tracker->moveNext();
            tracker->open();
        }

        return *tracker;
    }

    int IndexTracker::index() const { return m_index; }

    void IndexTracker::moveNext() {
        m_index++;
        m_children.clear();
    }

    void IndexTracker::close() {
        TrackerBase::close();
        if( m_runState == CompletedSuccessfully && m_index < m_size-1 )
            m_runState = Executing;
    }

} // namespace TestCaseTracking

using TestCaseTracking::ITracker;
using TestCaseTracking::TrackerContext;
using TestCaseTracking::SectionTracker;
using TestCaseTracking::IndexTracker;

} // namespace Catch

#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
